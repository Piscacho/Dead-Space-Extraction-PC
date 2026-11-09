// The game disc (disc.cpp), read from the extracted data partition.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// partitionRoot: folder with sys/ and files/ (e.g. "G:/Dead Space Extraction PC/juego/DATA").
bool DiscInit(const std::string& partitionRoot);
// The FST (sys/fst.bin), which the boot code leaves in memory for DVDOpen.
const std::vector<uint8_t>& DiscFst();
// Reads partition-relative bytes. Areas with no file read as zeros.
bool DiscRead(uint64_t offset, uint32_t length, uint8_t* dest);
