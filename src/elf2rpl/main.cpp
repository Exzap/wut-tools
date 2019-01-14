#include "elf.h"
#include "utils.h"

#include <algorithm>
#include <excmd.h>
#include <fmt/format.h>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <zlib.h>

constexpr auto DeflateMinSectionSize = 0x18u;
constexpr auto CodeBaseAddress = 0x02000000u;
constexpr auto DataBaseAddress = 0x10000000u;
constexpr auto LoadBaseAddress = 0xC0000000u;

struct ElfFile
{
   struct Section
   {
      elf::SectionHeader header;
      std::string name;
      std::vector<char> data;
   };

   elf::Header header;
   std::vector<std::unique_ptr<Section>> sections;
};

static int
getSectionIndex(ElfFile &file, const char *name)
{
   int index = 0;
   for (const auto &section : file.sections) {
      if (section->name == name) {
         return index;
      }

      ++index;
   }

   return -1;
}


static ElfFile::Section *
getSectionByType(ElfFile &file, elf::SectionType type)
{
   for (const auto &section : file.sections) {
      if (section->header.type == type) {
         return section.get();
      }
   }

   return nullptr;
}


/**
 * Read the .elf file generated by compiler.
 */
static bool
readElf(ElfFile &file, const std::string &filename)
{
   std::ifstream in { filename, std::ifstream::binary };
   if (!in.is_open()) {
      fmt::print("Could not open {} for reading\n", filename);
      return false;
   }

   // Read header
   in.read(reinterpret_cast<char *>(&file.header), sizeof(elf::Header));

   if (file.header.magic != elf::HeaderMagic) {
      fmt::print("Invalid ELF magic header {:08X}\n", elf::HeaderMagic);
      return false;
   }

   if (file.header.fileClass != elf::ELFCLASS32) {
      fmt::print("Unexpected ELF file class {}, expected {}\n", file.header.fileClass, elf::ELFCLASS32);
      return false;
   }

   if (file.header.encoding != elf::ELFDATA2MSB) {
      fmt::print("Unexpected ELF encoding {}, expected {}\n", file.header.encoding, elf::ELFDATA2MSB);
      return false;
   }

   if (file.header.machine != elf::EM_PPC) {
      fmt::print("Unexpected ELF machine type {}, expected {}\n", file.header.machine, elf::EM_PPC);
      return false;
   }

   if (file.header.elfVersion != elf::EV_CURRENT) {
      fmt::print("Unexpected ELF version {}, expected {}\n", file.header.elfVersion, elf::EV_CURRENT);
      return false;
   }

   // Read section headers and data
   in.seekg(static_cast<size_t>(file.header.shoff));

   for (auto i = 0u; i < file.header.shnum; ++i) {
      file.sections.emplace_back(std::make_unique<ElfFile::Section>());
      auto &section = *file.sections.back();

      in.read(reinterpret_cast<char *>(&section.header), sizeof(elf::SectionHeader));

      if (!section.header.size || section.header.type == elf::SHT_NOBITS) {
         continue;
      }

      auto pos = in.tellg();
      in.seekg(static_cast<size_t>(section.header.offset));
      section.data.resize(section.header.size);
      in.read(section.data.data(), section.data.size());
      in.seekg(pos);
   }

   // Set section header names
   auto shStrTab = file.sections[file.header.shstrndx]->data.data();

   for (auto &section : file.sections) {
      section->name = shStrTab + section->header.name;
   }

   return true;
}


/**
 * Generate SHT_RPL_FILEINFO section.
 */
static bool
generateFileInfoSection(ElfFile &file,
                        uint32_t flags)
{
   elf::RplFileInfo info;
   info.version = 0xCAFE0402u;
   info.textSize = 0u;
   info.textAlign = 32u;
   info.dataSize = 0u;
   info.dataAlign = 4096u;
   info.loadSize = 0u;
   info.loadAlign = 4u;
   info.tempSize = 0u;
   info.trampAdjust = 0u;
   info.trampAddition = 0u;
   info.sdaBase = 0u;
   info.sda2Base = 0u;
   info.stackSize = 0x10000u;
   info.heapSize = 0x8000u;
   info.filename = 0u;
   info.flags = flags;
   info.minVersion = 0x5078u;
   info.compressionLevel = 6;
   info.fileInfoPad = 0u;
   info.cafeSdkVersion = 0x5335u;
   info.cafeSdkRevision = 0x10D4Bu;
   info.tlsAlignShift = uint16_t { 0u };
   info.tlsModuleIndex = uint16_t { 0u };
   info.runtimeFileInfoSize = 0u;
   info.tagOffset = 0u;

   // Count file info textSize, dataSize, loadSize
   for (auto &section : file.sections) {
      auto size = static_cast<uint32_t>(section->data.size());

      if (section->header.type == elf::SHT_NOBITS) {
         size = section->header.size;
      }

      if (section->header.addr >= CodeBaseAddress &&
          section->header.addr < DataBaseAddress) {
         auto val = section->header.addr + section->header.size - CodeBaseAddress;
         if (val > info.textSize) {
            info.textSize = val;
         }
      } else if (section->header.addr >= DataBaseAddress &&
                 section->header.addr < LoadBaseAddress) {
         auto val = section->header.addr + section->header.size - DataBaseAddress;
         if (val > info.dataSize) {
            info.dataSize = val;
         }
      } else if (section->header.addr >= LoadBaseAddress) {
         auto val = section->header.addr + section->header.size - LoadBaseAddress;
         if (val > info.loadSize) {
            info.loadSize = val;
         }
      } else if (section->header.addr == 0 &&
                 section->header.type != elf::SHT_RPL_CRCS &&
                 section->header.type != elf::SHT_RPL_FILEINFO) {
         info.tempSize += (size + 128);
      }
   }

   info.textSize = align_up(info.textSize, info.textAlign);
   info.dataSize = align_up(info.dataSize, info.dataAlign);
   info.loadSize = align_up(info.loadSize, info.loadAlign);

   auto section = std::make_unique<ElfFile::Section>();
   section->header.name = 0u;
   section->header.type = elf::SHT_RPL_FILEINFO;
   section->header.flags = 0u;
   section->header.addr = 0u;
   section->header.offset = 0u;
   section->header.size = 0u;
   section->header.link = 0u;
   section->header.info = 0u;
   section->header.addralign = 4u;
   section->header.entsize = 0u;
   section->data.insert(section->data.end(),
                        reinterpret_cast<char *>(&info),
                        reinterpret_cast<char *>(&info + 1));
   file.sections.emplace_back(std::move(section));
   return true;
}


/**
 * Generate SHT_RPL_CRCS section.
 */
static bool
generateCrcSection(ElfFile &file)
{
   std::vector<be_val<uint32_t>> crcs;
   for (auto &section : file.sections) {
      auto crc = uint32_t { 0u };

      if (section->data.size()) {
         crc = crc32(0, Z_NULL, 0);
         crc = crc32(crc, reinterpret_cast<Bytef *>(section->data.data()), section->data.size());
      }

      crcs.push_back(crc);
   }

   // Insert a 0 crc for this section
   crcs.insert(crcs.end() - 1, 0);

   auto section = std::make_unique<ElfFile::Section>();
   section->header.name = 0u;
   section->header.type = elf::SHT_RPL_CRCS;
   section->header.flags = 0u;
   section->header.addr = 0u;
   section->header.offset = 0u;
   section->header.size = 0u;
   section->header.link = 0u;
   section->header.info = 0u;
   section->header.addralign = 4u;
   section->header.entsize = 4u;
   section->data.insert(section->data.end(),
                        reinterpret_cast<char *>(crcs.data()),
                        reinterpret_cast<char *>(crcs.data() + crcs.size()));

   // Insert before FILEINFO
   file.sections.insert(file.sections.end() - 1, std::move(section));
   return true;
}


static bool
getSymbol(ElfFile::Section &section,
          size_t index,
          elf::Symbol &symbol)
{
   auto symbols = reinterpret_cast<elf::Symbol *>(section.data.data());
   auto numSymbols = section.data.size() / sizeof(elf::Symbol);
   if (index >= numSymbols) {
      return false;
   }

   symbol = symbols[index];
   return true;
}


/**
 * Fix relocations.
 *
 * The Wii U does not support every type of relocation.
 */
static bool
fixRelocations(ElfFile &file)
{
   std::set<unsigned int> unsupportedTypes;
   auto result = true;

   for (auto &section : file.sections) {
      std::vector<elf::Rela> newRelocations;

      if (section->header.type != elf::SHT_RELA) {
         continue;
      }

      // Clear flags
      section->header.flags = 0u;

      auto &symbolSection = file.sections[section->header.link];
      auto &targetSection = file.sections[section->header.info];

      auto rels = reinterpret_cast<elf::Rela *>(section->data.data());
      auto numRels = section->data.size() / sizeof(elf::Rela);
      for (auto i = 0u; i < numRels; ++i) {
         auto info = rels[i].info;
         auto addend = rels[i].addend;
         auto offset = rels[i].offset;
         auto index = info >> 8;
         auto type = info & 0xFF;

         switch (type) {
         case elf::R_PPC_NONE:
         case elf::R_PPC_ADDR32:
         case elf::R_PPC_ADDR16_LO:
         case elf::R_PPC_ADDR16_HI:
         case elf::R_PPC_ADDR16_HA:
         case elf::R_PPC_REL24:
         case elf::R_PPC_REL14:
         case elf::R_PPC_DTPMOD32:
         case elf::R_PPC_DTPREL32:
         case elf::R_PPC_EMB_SDA21:
         case elf::R_PPC_EMB_RELSDA:
         case elf::R_PPC_DIAB_SDA21_LO:
         case elf::R_PPC_DIAB_SDA21_HI:
         case elf::R_PPC_DIAB_SDA21_HA:
         case elf::R_PPC_DIAB_RELSDA_LO:
         case elf::R_PPC_DIAB_RELSDA_HI:
         case elf::R_PPC_DIAB_RELSDA_HA:
            // All valid relocations on Wii U, do nothing
            break;

         /*
          * Convert a R_PPC_REL32 into two GHS_REL16
          */
         case elf::R_PPC_REL32:
         {
            elf::Symbol symbol;
            if (!getSymbol(*symbolSection, index, symbol)) {
               fmt::print("ERROR: Could not find symbol {} for fixing a R_PPC_REL32 relocation\n", index);
               result = false;
            } else {
               newRelocations.emplace_back();
               auto &newRel = newRelocations.back();

               // Modify current relocation to R_PPC_GHS_REL16_HI
               rels[i].info = (index << 8) | elf::R_PPC_GHS_REL16_HI;
               rels[i].addend = addend;
               rels[i].offset = offset;

               // Create a R_PPC_GHS_REL16_LO
               newRel.info = (index << 8) | elf::R_PPC_GHS_REL16_LO;
               newRel.addend = addend + 2;
               newRel.offset = offset + 2;
            }

            break;
         }

         default:
            // Only print error once per type
            if (!unsupportedTypes.count(type)) {
               fmt::print("ERROR: Unsupported relocation type {}\n", type);
               unsupportedTypes.insert(type);
            }
         }
      }

      section->data.insert(section->data.end(),
                           reinterpret_cast<char *>(newRelocations.data()),
                           reinterpret_cast<char *>(newRelocations.data() + newRelocations.size()));
   }

   return result && unsupportedTypes.size() == 0;
}


/**
 * Fix file header to look like an RPL file!
 */
static bool
fixFileHeader(ElfFile &file)
{
   file.header.magic = elf::HeaderMagic;
   file.header.fileClass = uint8_t { 1 };
   file.header.encoding = elf::ELFDATA2MSB;
   file.header.elfVersion = elf::EV_CURRENT;
   file.header.abi = elf::EABI_CAFE;
   memset(&file.header.pad, 0, 7);
   file.header.type = uint16_t { 0xFE01 };
   file.header.machine = elf::EM_PPC;
   file.header.version = 1u;
   file.header.flags = 0u;
   file.header.phoff = 0u;
   file.header.phentsize = uint16_t { 0 };
   file.header.phnum = uint16_t { 0 };
   file.header.shoff = align_up(static_cast<uint32_t>(sizeof(elf::Header)), 64);
   file.header.shnum = static_cast<uint16_t>(file.sections.size());
   file.header.shentsize = static_cast<uint16_t>(sizeof(elf::SectionHeader));
   file.header.ehsize = static_cast<uint16_t>(sizeof(elf::Header));
   file.header.shstrndx = static_cast<uint16_t>(getSectionIndex(file, ".shstrtab"));
   return true;
}


/**
 * Relocate a section to a new address.
 */
static bool
relocateSection(ElfFile &file,
                ElfFile::Section &section,
                uint32_t sectionIndex,
                uint32_t newSectionAddress)
{
   auto sectionSize = section.data.size() ? section.data.size() : static_cast<size_t>(section.header.size);
   auto oldSectionAddress = section.header.addr;
   auto oldSectionAddressEnd = section.header.addr + sectionSize;

   // Relocate symbols pointing into this section
   for (auto &symSection : file.sections) {
      if (symSection->header.type != elf::SectionType::SHT_SYMTAB) {
         continue;
      }

      auto symbols = reinterpret_cast<elf::Symbol *>(symSection->data.data());
      auto numSymbols = symSection->data.size() / sizeof(elf::Symbol);
      for (auto i = 0u; i < numSymbols; ++i) {
         auto type = symbols[i].info & 0xf;
         auto value = symbols[i].value;

         // Only relocate data, func, section symbols
         if (type != elf::STT_OBJECT &&
             type != elf::STT_FUNC &&
             type != elf::STT_SECTION) {
            continue;
         }

         if (value >= oldSectionAddress && value <= oldSectionAddressEnd) {
            symbols[i].value = (value - oldSectionAddress) + newSectionAddress;
         }
      }
   }

   // Relocate relocations pointing into this section
   for (auto &relaSection : file.sections) {
      if (relaSection->header.type != elf::SectionType::SHT_RELA ||
          relaSection->header.info != sectionIndex) {
         continue;
      }

      auto rela = reinterpret_cast<elf::Rela *>(relaSection->data.data());
      auto numRelas = relaSection->data.size() / sizeof(elf::Rela);
      for (auto i = 0u; i < numRelas; ++i) {
         auto offset = rela[i].offset;

         if (offset >= oldSectionAddress && offset <= oldSectionAddressEnd) {
            rela[i].offset = (offset - oldSectionAddress) + newSectionAddress;
         }
      }
   }

   section.header.addr = newSectionAddress;
   return true;
}


/**
 * Fix the loader virtual addresses.
 *
 * Linker script won't put symtab & strtab sections in our loader address, so
 * we must fix that.
 */
static bool
fixLoaderVirtualAddresses(ElfFile &file)
{
   auto loadMax = LoadBaseAddress;
   for (auto &section : file.sections) {
      if (section->header.addr >= loadMax) {
         loadMax = section->header.addr + section->data.size();
      }
   }

   // Relocate .symtab and .strtab to be in loader memory
   for (auto i = 0u; i < file.sections.size(); ++i) {
      auto &section = file.sections[i];
      if (section->header.type == elf::SHT_SYMTAB ||
          section->header.type == elf::SHT_STRTAB) {
         relocateSection(file, *section, i,
                         align_up(loadMax, section->header.addralign));
         section->header.flags |= elf::SHF_ALLOC;
         loadMax += section->data.size();
      }
   }

   return true;
}


/**
 * zlib deflate any suitable section.
 */
static bool
deflateSections(ElfFile &file)
{
   std::vector<char> chunk;
   chunk.resize(16 * 1024);

   for (auto &section : file.sections) {
      if (section->data.size() < DeflateMinSectionSize ||
          section->header.type == elf::SHT_RPL_CRCS ||
          section->header.type == elf::SHT_RPL_FILEINFO) {
         continue;
      }

      // Allocate space for the 4 bytes inflated size
      std::vector<char> deflated;
      deflated.resize(4);

      // Deflate section data
      auto stream = z_stream { };
      memset(&stream, 0, sizeof(stream));
      stream.zalloc = Z_NULL;
      stream.zfree = Z_NULL;
      stream.opaque = Z_NULL;
      deflateInit(&stream, 6);

      stream.avail_in = section->data.size();
      stream.next_in = reinterpret_cast<Bytef *>(section->data.data());

      do {
         stream.avail_out = static_cast<uInt>(chunk.size());
         stream.next_out = reinterpret_cast<Bytef *>(chunk.data());

         auto ret = deflate(&stream, Z_FINISH);
         if (ret == Z_STREAM_ERROR) {
            deflateEnd(&stream);
            return false;
         }

         deflated.insert(deflated.end(),
                         chunk.data(),
                         reinterpret_cast<char *>(stream.next_out));
      } while (stream.avail_out == 0);
      deflateEnd(&stream);

      // Set the inflated size at start of section
      *reinterpret_cast<be_val<uint32_t> *>(&deflated[0]) =
         static_cast<uint32_t>(section->data.size());

      // Update the section data
      section->data = std::move(deflated);
      section->header.flags |= elf::SHF_DEFLATED;
   }

   return true;
}


/**
 * Calculate section file offsets.
 *
 * Expected order:
 * RPL_CRCS > RPL_FILEINFO >
 * Data sections > Read sections > Text sections > Temp sections
 */
static bool
calculateSectionOffsets(ElfFile &file)
{
   auto offset = file.header.shoff;
   offset += align_up(static_cast<uint32_t>(file.sections.size() * sizeof(elf::SectionHeader)), 64);

   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_NOBITS ||
          section->header.type == elf::SHT_NULL) {
         section->header.offset = 0u;
         section->data.clear();
      }
   }

   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_RPL_CRCS) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_RPL_FILEINFO) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // First the "dataMin / dataMax" sections, which are:
   // - !(flags & SHF_EXECINSTR)
   // - flags & SHF_WRITE
   // - flags & SHF_ALLOC
   for (auto &section : file.sections) {
      if (section->header.size == 0 ||
          section->header.type == elf::SHT_RPL_FILEINFO ||
          section->header.type == elf::SHT_RPL_IMPORTS ||
          section->header.type == elf::SHT_RPL_CRCS ||
          section->header.type == elf::SHT_NOBITS) {
         continue;
      }

      if (!(section->header.flags & elf::SHF_EXECINSTR) &&
           (section->header.flags & elf::SHF_WRITE) &&
           (section->header.flags & elf::SHF_ALLOC)) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Next the "readMin / readMax" sections, which are:
   // - !(flags & SHF_EXECINSTR) || type == SHT_RPL_EXPORTS
   // - !(flags & SHF_WRITE)
   // - flags & SHF_ALLOC
   for (auto &section : file.sections) {
      if (section->header.size == 0 ||
          section->header.type == elf::SHT_RPL_FILEINFO ||
          section->header.type == elf::SHT_RPL_IMPORTS ||
          section->header.type == elf::SHT_RPL_CRCS ||
          section->header.type == elf::SHT_NOBITS) {
         continue;
      }

      if ((!(section->header.flags & elf::SHF_EXECINSTR) ||
             section->header.type == elf::SHT_RPL_EXPORTS) &&
          !(section->header.flags & elf::SHF_WRITE) &&
           (section->header.flags & elf::SHF_ALLOC)) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Import sections are part of the read sections, but have execinstr flag set
   // so let's insert them here to avoid complicating the above logic.
   for (auto &section : file.sections) {
      if (section->header.type == elf::SHT_RPL_IMPORTS) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Next the "textMin / textMax" sections, which are:
   // - flags & SHF_EXECINSTR
   // - type != SHT_RPL_EXPORTS
   for (auto &section : file.sections) {
      if (section->header.size == 0 ||
          section->header.type == elf::SHT_RPL_FILEINFO ||
          section->header.type == elf::SHT_RPL_IMPORTS ||
          section->header.type == elf::SHT_RPL_CRCS ||
          section->header.type == elf::SHT_NOBITS) {
         continue;
      }

      if ((section->header.flags & elf::SHF_EXECINSTR) &&
           section->header.type != elf::SHT_RPL_EXPORTS) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   // Next the "tempMin / tempMax" sections, which are:
   // - !(flags & SHF_EXECINSTR)
   // - !(flags & SHF_ALLOC)
   for (auto &section : file.sections) {
      if (section->header.size == 0 ||
          section->header.type == elf::SHT_RPL_FILEINFO ||
          section->header.type == elf::SHT_RPL_IMPORTS ||
          section->header.type == elf::SHT_RPL_CRCS ||
          section->header.type == elf::SHT_NOBITS) {
         continue;
      }

      if (!(section->header.flags & elf::SHF_EXECINSTR) &&
          !(section->header.flags & elf::SHF_ALLOC)) {
         section->header.offset = offset;
         section->header.size = static_cast<uint32_t>(section->data.size());
         offset += section->header.size;
      }
   }

   auto index = 0u;
   for (auto &section : file.sections) {
      if (section->header.offset == 0 &&
          section->header.type != elf::SHT_NULL &&
          section->header.type != elf::SHT_NOBITS) {
         fmt::print("Failed to calculate offset for section {}\n", index);
         return false;
      }

      ++index;
   }

   return true;
}


/**
 * Write out the final RPL.
 */
static bool
writeRpl(ElfFile &file, const std::string &filename)
{
   auto shoff = file.header.shoff;

   // Write the file out
   std::ofstream out { filename, std::ofstream::binary };

   if (!out.is_open()) {
      fmt::print("Could not open {} for writing\n", filename);
      return false;
   }

   // Write file header
   out.seekp(0, std::ios::beg);
   out.write(reinterpret_cast<const char *>(&file.header), sizeof(elf::Header));

   // Write section headers
   out.seekp(shoff, std::ios::beg);
   for (const auto &section : file.sections) {
      out.write(reinterpret_cast<const char *>(&section->header), sizeof(elf::SectionHeader));
   }

   // Write sections
   for (const auto &section : file.sections) {
      if (section->data.size()) {
         out.seekp(section->header.offset, std::ios::beg);
         out.write(section->data.data(), section->data.size());
      }
   }

   return true;
}

int main(int argc, char **argv)
{
   excmd::parser parser;
   excmd::option_state options;
   using excmd::description;
   using excmd::value;

   try {
      parser.global_options()
         .add_option("H,help",
                     description { "Show help." })
         .add_option("r,rpl",
                     description { "Generate an RPL instead of an RPX" });

      parser.default_command()
         .add_argument("src",
                       description { "Path to input elf file" },
                       value<std::string> {})
         .add_argument("dst",
                       description { "Path to output rpl file" },
                       value<std::string> {});

      options = parser.parse(argc, argv);
   } catch (excmd::exception ex) {
      fmt::print("Error parsing options: {}\n", ex.what());
      return -1;
   }

   if (options.empty()
       || options.has("help")
       || !options.has("src")
       || !options.has("dst")) {
      fmt::print("{} <options> src dst\n", argv[0]);
      fmt::print("{}\n", parser.format_help(argv[0]));
      return 0;
   }

   auto src = options.get<std::string>("src");
   auto dst = options.get<std::string>("dst");
   auto isRpl = options.has("rpl");

   // Read elf into memory object!
   ElfFile elf;

   if (!readElf(elf, src)) {
      fmt::print("ERROR: readElf failed.\n");
      return -1;
   }

   if (!fixRelocations(elf)) {
      fmt::print("ERROR: fixRelocations failed.\n");
      return -1;
   }

   if (!fixLoaderVirtualAddresses(elf)) {
      fmt::print("ERROR: fixLoaderVirtualAddresses failed.\n");
      return -1;
   }

   if (!generateFileInfoSection(elf, isRpl ? 0 : elf::RPL_IS_RPX)) {
      fmt::print("ERROR: generateFileInfoSection failed.\n");
      return -1;
   }

   if (!generateCrcSection(elf)) {
      fmt::print("ERROR: generateCrcSection failed.\n");
      return -1;
   }

   if (!fixFileHeader(elf)) {
      fmt::print("ERROR: fixFileHeader faile.\n");
      return -1;
   }

   if (!deflateSections(elf)) {
      fmt::print("ERROR: deflateSections failed.\n");
      return -1;
   }

   if (!calculateSectionOffsets(elf)) {
      fmt::print("ERROR: calculateSectionOffsets failed.\n");
      return -1;
   }

   if (!writeRpl(elf, dst)) {
      fmt::print("ERROR: writeRpl failed.\n");
      return -1;
   }

   return 0;
}