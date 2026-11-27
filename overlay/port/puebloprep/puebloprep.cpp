// PuebloPrep - prepares the SD card for PuebloCTR (G4BP08 / G4BE08, GameCube -> New Nintendo 3DS) from the player's own
// GameCube discs. R5 of the release plan; this is the first step: the console part.
//
//   puebloprep-cli --in DISC1.iso --in DISC2.iso [--out DIR]       unpack both discs to DIR/3ds/pueblo/data/disc0, disc1
//   puebloprep-cli --in DISC.iso ... --check DIR                   compare a prepared folder with the images (sizes and bytes)
// DIR is a folder on the computer (default PuebloCTR-SD); the player copies its 3ds folder to the root of the SD card
// themselves - with a card reader or over FTP (the owner's choice, 2026-11-16: PuebloPrep does not write to the card).
//   puebloprep-cli --in DISC.iso --info                            what the image is
//
// The layout is the one the port reads (runtime/dvd.cpp; Dolphin's "Extract Entire Disc"):
//   data/discN/sys/boot.bin bi2.bin apploader.img main.dol fst.bin
//   data/discN/files/<every file of the disc's file system table>
// discN is the disc's own number from its header (byte 6), so the order of --in does not matter.
//
// Editions (the same rule as runtime/dvd.cpp and the start's check, 3ds/startcheck_3ds.cpp): G4BP08 the European
// release, G4BE08 with the file system table elsewhere than 0x28FE00 the US release - both played by the beta;
// G4BJ08 the Japanese release - played too (stage 69, 2026-11-21); G4BE08 with the table at 0x28FE00 the debug
// build, anything else - refused.
//
// Images: plain GameCube ones (.iso / .gcm, 1 459 978 240 bytes) and RVZ (Dolphin; through a temporary plain image in
// the system's temporary folder). Windows, macOS, Linux (std::filesystem; ffmpeg through the shell). Nothing of the game
// is in this program. MIT licence (the repository's LICENSE).
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "re4_card_files.h"  // CubeEvil4's .gci (runtime/gci.cpp, card_files.cpp), built in by puebloprep_build.sh
#include "disc_rvz.h"  // CubeEvil4's RVZ reader (port/android-decomp/app/src/main/cpp; zstd), built in by puebloprep_build.sh

// Windows, macOS, Linux: the file system through std::filesystem; 64-bit file offsets; the shell's null device and
// quoting for ffmpeg (cmd.exe: the whole command line once more in quotes).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // CreateProcess (ffmpeg without a console window)
#include <fcntl.h>  // _O_BINARY for _fmode
#include <stdlib.h>
#define fseeko _fseeki64
#define ftello _ftelli64
#endif

namespace {

namespace fs = std::filesystem;

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#ifdef _WIN32
const char* const kNullDevice = "NUL";
#else
const char* const kNullDevice = "/dev/null";
#endif

fs::path native(const std::string& utf8)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
}

// The size of a regular file, false when there is none.
bool fileSize(const std::string& path, u64* size)
{
    std::error_code ec;
    const fs::path p = native(path);
    if (!fs::is_regular_file(p, ec)) {
        return false;
    }
    const auto n = fs::file_size(p, ec);
    if (ec) {
        return false;
    }
    *size = (u64) n;
    return true;
}

// The bytes free where `path` lies (0: unknown).
u64 freeBytes(const std::string& path)
{
    std::error_code ec;
    const fs::space_info s = fs::space(native(path), ec);
    return ec ? 0 : (u64) s.available;
}

bool seekTo(FILE* f, u64 at)
{
    return fseeko(f, (decltype(ftello(f))) at, SEEK_SET) == 0;
}

#ifndef PUEBLOPREP_VERSION  // (set by the build: the release tag)
#define PUEBLOPREP_VERSION "dev"
#endif
const char* const kVersion = "PuebloPrep " PUEBLOPREP_VERSION;
const u64 kImageBytes = 1459978240ull;  // a GameCube disc
const u32 kDebugFstOffset = 0x28FE00;
const u32 kAppLoader = 0x2440;

FILE* gLog;
void (*gWindowSay)(const char*);  // (the window, puebloprep_gui.cpp) each line also there; fail() throws
std::vector<std::string> gTemps;  // files removed at the end, also after an error (atexit)

void removeTemps()
{
    for (const std::string& p : gTemps) {
        std::error_code ec;
        fs::remove(native(p), ec);
    }
}

void say(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    char line[2048];
    vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    if (gWindowSay) {
        gWindowSay(line);
    }
    if (gLog) {
        fputs(line, gLog);
        fputc('\n', gLog);
        fflush(gLog);
    }
}

[[noreturn]] void fail(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    char line[2048];
    vsnprintf(line, sizeof(line), fmt, a);
    va_end(a);
    say("error: %s", line);
    if (gWindowSay) {
        throw std::runtime_error(line);
    }
    exit(1);
}

u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

enum Edition { kEu, kUs, kDebug, kJp, kUnknown };
const char* const kEditionName[] = {"European release (G4BP08)", "US release (G4BE08)", "debug build (G4BE08)",
                                    "Japanese release (G4BJ08)", "unknown game"};

struct Entry {
    std::string path;  // inside files/, '/' between the parts
    u32 offset, size;
};

struct Image {
    std::string path;
    FILE* f = nullptr;
    u64 bytes = 0;
    std::vector<u8> header;  // boot.bin: the first 0x440 bytes
    Edition edition = kUnknown;
    int discNo = -1;
    u32 dolOffset = 0, dolBytes = 0, fstOffset = 0, fstBytes = 0, appBytes = 0;
    std::vector<u8> fst;
    std::vector<Entry> files;
    u64 fileBytes = 0;

    void read(u64 at, void* dst, size_t n) const
    {
        if (at + n > bytes) {
            fail("%s: a read of %zu bytes at 0x%" PRIx64 " past the image's end", path.c_str(), n, at);
        }
        if (!seekTo(f, at) || fread(dst, 1, n, f) != n) {
            fail("%s: cannot read %zu bytes at 0x%" PRIx64 " (%s)", path.c_str(), n, at, strerror(errno));
        }
    }
    std::vector<u8> bytesAt(u64 at, size_t n) const
    {
        std::vector<u8> v(n);
        if (n) {
            read(at, v.data(), n);
        }
        return v;
    }
};

Edition editionOf(const u8* boot)
{
    const u32 fstOffset = be32(boot + 0x424);
    if (!memcmp(boot, "G4BP08", 6)) {
        return kEu;
    }
    if (!memcmp(boot, "G4BE08", 6)) {
        return fstOffset == kDebugFstOffset ? kDebug : kUs;
    }
    if (!memcmp(boot, "G4BJ08", 6)) {
        return kJp;
    }
    return kUnknown;
}

// The size of a DOL: the end of its furthest section (7 text, 11 data; offsets at 0x00, sizes at 0x90).
u32 dolSize(const Image& im)
{
    u8 h[0x100];
    im.read(im.dolOffset, h, sizeof(h));
    u32 end = 0x100;
    for (int i = 0; i < 18; i++) {
        const u32 off = be32(h + i * 4), size = be32(h + 0x90 + i * 4);
        if (off && size && off + size > end) {
            end = off + size;
        }
    }
    return end;
}

// The file system table: 12-byte entries (flags + name offset, offset or parent, size or next), names after them.
void parseFst(Image& im)
{
    const std::vector<u8>& t = im.fst;
    if (t.size() < 12 || t[0] != 1) {
        fail("%s: the file system table does not start with its root folder", im.path.c_str());
    }
    const u32 count = be32(&t[8]);
    if ((u64) count * 12 > t.size()) {
        fail("%s: the file system table claims %u entries", im.path.c_str(), count);
    }
    const size_t names = (size_t) count * 12;
    struct Dir {
        u32 end;
        std::string prefix;
    };
    std::vector<Dir> stack{{count, ""}};
    for (u32 i = 1; i < count; i++) {
        while (stack.size() > 1 && i >= stack.back().end) {
            stack.pop_back();
        }
        const u8* e = &t[i * 12];
        const u32 nameAt = be32(e) & 0xFFFFFF;
        if (names + nameAt >= t.size()) {
            fail("%s: entry %u's name lies outside the table", im.path.c_str(), i);
        }
        const char* name = (const char*) &t[names + nameAt];
        const size_t len = strnlen(name, t.size() - names - nameAt);
        std::string n(name, len);
        if (n.empty() || n == "." || n == ".." || n.find('/') != std::string::npos ||
            n.find('\\') != std::string::npos) {
            fail("%s: entry %u has an unsafe name", im.path.c_str(), i);
        }
        if (e[0]) {
            const u32 next = be32(e + 8);
            if (next <= i || next > count) {
                fail("%s: folder entry %u ends at %u", im.path.c_str(), i, next);
            }
            stack.push_back({next, stack.back().prefix + n + "/"});
        } else {
            Entry f{stack.back().prefix + n, be32(e + 4), be32(e + 8)};
            if ((u64) f.offset + f.size > im.bytes) {
                fail("%s: %s lies past the image's end", im.path.c_str(), f.path.c_str());
            }
            im.fileBytes += f.size;
            im.files.push_back(std::move(f));
        }
    }
}

Image openImage(const std::string& path)
{
    Image im;
    im.path = path;
    im.f = fopen(path.c_str(), "rb");
    if (!im.f) {
        fail("%s: cannot open (%s)", path.c_str(), strerror(errno));
    }
    fseeko(im.f, 0, SEEK_END);
    im.bytes = (u64) ftello(im.f);
    if (im.bytes < 0x10000) {
        fail("%s: %" PRIu64 " bytes is no GameCube disc", path.c_str(), im.bytes);
    }
    im.header = im.bytesAt(0, 0x440);
    if (be32(&im.header[0x1C]) != 0xC2339F3Du) {
        fail("%s: no GameCube disc header (CISO, GCZ, WIA and other compressed images are not read: convert to "
             "ISO or RVZ with Dolphin, README \"Troubleshooting\")", path.c_str());
    }
    if (im.bytes != kImageBytes) {
        say("warning: %s: %" PRIu64 " bytes, a full GameCube disc has %" PRIu64 " (a trimmed image?)", path.c_str(),
            im.bytes, kImageBytes);
    }
    im.edition = editionOf(im.header.data());
    im.discNo = im.header[6];
    im.dolOffset = be32(&im.header[0x420]);
    im.fstOffset = be32(&im.header[0x424]);
    im.fstBytes = be32(&im.header[0x428]);
    if (im.fstBytes == 0 || im.fstBytes > (8u << 20)) {
        fail("%s: a file system table of %u bytes", path.c_str(), im.fstBytes);
    }
    im.fst = im.bytesAt(im.fstOffset, im.fstBytes);
    im.dolBytes = dolSize(im);
    u8 app[32];
    im.read(kAppLoader, app, sizeof(app));
    im.appBytes = 32 + be32(app + 20) + be32(app + 24);
    parseFst(im);
    return im;
}

void describe(const Image& im)
{
    char title[0x41] = {};
    memcpy(title, &im.header[0x20], 0x40);
    say("%s: %s, disc %d, \"%s\", %zu files (%.1f MB)", im.path.c_str(), kEditionName[im.edition], im.discNo + 1,
        title, im.files.size(), im.fileBytes / 1048576.0);
}

// The beta plays the European, the US and the Japanese release, both discs of one edition, each disc once.
void checkPair(const std::vector<Image>& ims)
{
    for (const Image& im : ims) {
        if (im.edition == kDebug || im.edition == kUnknown) {
            fail("%s: the %s is not played by this beta (the European, the US and the Japanese release are)", im.path.c_str(),
                 kEditionName[im.edition]);
        }
        if (im.discNo != 0 && im.discNo != 1) {
            fail("%s: disc number %d", im.path.c_str(), im.discNo + 1);
        }
    }
    for (size_t i = 0; i < ims.size(); i++) {
        for (size_t j = i + 1; j < ims.size(); j++) {
            if (ims[i].discNo == ims[j].discNo) {
                fail("disc %d given twice (%s, %s)", ims[i].discNo + 1, ims[i].path.c_str(), ims[j].path.c_str());
            }
            if (ims[i].edition != ims[j].edition) {
                fail("the discs are of different editions (%s: %s, %s: %s)", ims[i].path.c_str(),
                     kEditionName[ims[i].edition], ims[j].path.c_str(), kEditionName[ims[j].edition]);
            }
        }
    }
    bool has[2] = {};
    for (const Image& im : ims) {
        has[im.discNo] = true;
    }
    if (!has[0]) {
        fail("disc 1 is missing: the game starts from it");
    }
    if (!has[1]) {
        say("warning: no disc 2 - the game plays until it asks for the second disc (the island)");
    }
}

bool makeDirs(const std::string& path)
{
    std::error_code ec;
    fs::create_directories(native(path), ec);
    if (ec && !fs::is_directory(native(path))) {
        errno = ec.value();
        return false;
    }
    return true;
}

// FAT32 (the SD card): no file of 4 GB, names without the characters FAT refuses, a path short enough.
void checkFat(const Image& im)
{
    for (const Entry& e : im.files) {
        for (char c : e.path) {
            if (strchr("<>:\"\\|?*", c) || (unsigned char) c < 0x20) {
                fail("%s: %s has a character FAT32 refuses", im.path.c_str(), e.path.c_str());
            }
        }
        // the path on the card: /3ds/pueblo/data/discN/files/<path>
        if (std::strlen("/3ds/pueblo/data/disc0/files/") + e.path.size() > 250) {
            fail("%s: the path of %s is too long for FAT32", im.path.c_str(), e.path.c_str());
        }
    }
}

void writeFile(const std::string& path, const Image& im, u64 at, u32 size, std::vector<u8>& buf)
{
    const std::string part = path + ".part";
    FILE* o = fopen(part.c_str(), "wb");
    if (!o) {
        fail("cannot write %s (%s)", part.c_str(), strerror(errno));
    }
    u32 left = size;
    while (left) {
        const u32 n = left < buf.size() ? left : (u32) buf.size();
        im.read(at, buf.data(), n);
        if (fwrite(buf.data(), 1, n, o) != n) {
            fail("cannot write %s (%s; the card full?)", part.c_str(), strerror(errno));
        }
        at += n;
        left -= n;
    }
    if (fclose(o) != 0) {
        fail("cannot write %s (%s)", part.c_str(), strerror(errno));
    }
    remove(path.c_str());
    if (rename(part.c_str(), path.c_str()) != 0) {
        fail("cannot rename %s (%s)", part.c_str(), strerror(errno));
    }
}

void writeBytes(const std::string& path, const std::vector<u8>& d)
{
    FILE* o = fopen(path.c_str(), "wb");
    if (!o || fwrite(d.data(), 1, d.size(), o) != d.size() || fclose(o) != 0) {
        fail("cannot write %s (%s)", path.c_str(), strerror(errno));
    }
}

void unpack(const Image& im, const std::string& data)
{
    const std::string base = data + "/disc" + std::to_string(im.discNo);
    checkFat(im);
    if (!makeDirs(base + "/sys")) {
        fail("cannot make %s/sys (%s)", base.c_str(), strerror(errno));
    }
    writeBytes(base + "/sys/boot.bin", im.header);
    writeBytes(base + "/sys/bi2.bin", im.bytesAt(0x440, 0x2000));
    writeBytes(base + "/sys/apploader.img", im.bytesAt(kAppLoader, im.appBytes));
    writeBytes(base + "/sys/main.dol", im.bytesAt(im.dolOffset, im.dolBytes));
    writeBytes(base + "/sys/fst.bin", im.fst);
    std::vector<u8> buf(4u << 20);
    u64 done = 0;
    int shown = -1;
    for (size_t i = 0; i < im.files.size(); i++) {
        const Entry& e = im.files[i];
        const std::string path = base + "/files/" + e.path;
        const size_t slash = path.rfind('/');
        if (!makeDirs(path.substr(0, slash))) {
            fail("cannot make the folder of %s (%s)", path.c_str(), strerror(errno));
        }
        u64 have;
        if (fileSize(path, &have) && have == e.size) {
            done += e.size;  // there from an earlier run (sizes only: --check compares the bytes)
            continue;
        }
        writeFile(path, im, e.offset, e.size, buf);
        done += e.size;
        const int pc = (int) (done * 100 / (im.fileBytes ? im.fileBytes : 1));
        if (pc / 5 != shown / 5) {
            shown = pc;
            say("disc %d: %3d %%  %s", im.discNo + 1, pc, e.path.c_str());
        }
    }
    say("disc %d: %zu files -> %s", im.discNo + 1, im.files.size(), base.c_str());
}

int check(const Image& im, const std::string& data)
{
    const std::string base = data + "/disc" + std::to_string(im.discNo);
    int problems = 0;
    auto same = [&](const std::string& path, u64 at, u32 size) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) {
            say("missing: %s", path.c_str());
            return false;
        }
        std::vector<u8> a(1u << 20), b(1u << 20);
        u32 left = size;
        bool ok = true;
        fseeko(f, 0, SEEK_END);
        if ((u64) ftello(f) != size) {
            say("size differs: %s", path.c_str());
            ok = false;
        }
        fseeko(f, 0, SEEK_SET);
        while (ok && left) {
            const u32 n = left < a.size() ? left : (u32) a.size();
            im.read(at, a.data(), n);
            if (fread(b.data(), 1, n, f) != n || memcmp(a.data(), b.data(), n) != 0) {
                say("bytes differ: %s", path.c_str());
                ok = false;
            }
            at += n;
            left -= n;
        }
        fclose(f);
        return ok;
    };
    problems += !same(base + "/sys/boot.bin", 0, 0x440);
    problems += !same(base + "/sys/fst.bin", im.fstOffset, im.fstBytes);
    problems += !same(base + "/sys/main.dol", im.dolOffset, im.dolBytes);
    for (const Entry& e : im.files) {
        problems += !same(base + "/files/" + e.path, e.offset, e.size);
    }
    say("%s: disc %d, %zu files, %d problems", base.c_str(), im.discNo + 1, im.files.size(), problems);
    return problems;
}

// --- Yz2: the room archives (St*/rNNN.das) are packed with Capcom's yz2 - an adaptive range coder over a literal /
// dictionary-run alphabet. Written after the description of CubeEvil4's tools/motion/yz2.py (the format as the game's
// decoder reads it): a text header "<packed hex><sep><unpacked hex>", the coded bytes at the next 32-byte boundary;
// models of 0x500 (main) and 0x100 (side) symbols whose cumulative table totals 0x8000; a dictionary of 256 contexts
// (the byte before) x 512-entry rings of (where, length). An archive whose body is not packed starts with its entry
// count; a .das file's body starts at 0x400.
struct Yz2Model {
    int n;
    std::vector<u32> freq, cumF, cumS;
    u32 total;
    int bits;
    u32 range;
    explicit Yz2Model(int count) : n(count), freq(count, 1), cumF(count), cumS(count)
    {
        u32 run = 0;
        for (int i = 0; i < n; i++) {  // 0x8000 counts dealt round-robin
            cumF[i] = 0x8000 / n + ((u32) i < 0x8000u % n ? 1 : 0);
            cumS[i] = run;
            run += cumF[i];
        }
        total = n;
        bits = 0;
        do {
            bits++;
        } while (bits <= 14 && !(n < (1 << bits)));
        range = 1u << bits;
    }
};

bool yz2Header(const u8* d, size_t n, size_t base, u32* packed, u32* unpacked, size_t* start)
{
    size_t p = base;
    auto skipSpace = [&] { while (p < n && (d[p] == ' ' || d[p] == '\t' || d[p] == '\n' || d[p] == '\r')) p++; };
    auto hex = [&](u32* v) {
        size_t from = p;
        u64 x = 0;
        while (p < n && isxdigit(d[p])) {
            x = x * 16 + (u32) (isdigit(d[p]) ? d[p] - '0' : (tolower(d[p]) - 'a' + 10));
            p++;
        }
        *v = (u32) x;
        return p > from && p - from <= 8;
    };
    skipSpace();
    if (!hex(packed) || p >= n) {
        return false;
    }
    p++;  // the separator
    skipSpace();
    if (!hex(unpacked) || p - base > 32) {  // (the header within its first 32 bytes, as yz2.py looks for it)
        return false;
    }
    *start = (p + 0x20) & ~(size_t) 0x1F;
    return *start < n;
}

// The decompressed body, or empty with `why` set.
std::vector<u8> yz2Decode(const std::vector<u8>& in, size_t base, std::string* why)
{
    u32 packed, unpacked;
    size_t pos;
    if (!yz2Header(in.data(), in.size(), base, &packed, &unpacked, &pos)) {
        *why = "no yz2 header";
        return {};
    }
    (void) packed;
    std::vector<u8> out(unpacked);
    Yz2Model mainModel(0x500), side(0x100);
    const size_t inSize = in.size();
    u32 R = 0x80, C = in[pos++];
    std::vector<int64_t> dicPtr(256 * 512, -1);
    std::vector<u32> dicLen(256 * 512, 0);
    u32 dicCnt[256] = {};
    bool broken = false;
    auto next = [&]() -> u32 {
        if (pos >= inSize) {
            broken = true;
            return 0;
        }
        return in[pos++];
    };
    auto sym = [&](Yz2Model& m) -> u32 {
        if (R <= 0x800000) {
            if (R > 0x8000) {
                C = C << 8 | next();
                R <<= 8;
            } else if (R > 0x80) {
                const u32 a = next(), b = next();
                C = C << 16 | a << 8 | b;
                R <<= 16;
            } else {
                const u32 a = next(), b = next(), c = next();
                C = C << 24 | a << 16 | b << 8 | c;
                R <<= 24;
            }
        }
        const u32 step = R >> 14;
        if (!step) {
            broken = true;
            return 0;
        }
        const u32 value = C / step;
        if (value >= 0x8000) {
            broken = true;
            return 0;
        }
        // the last slot whose start <= value
        int lo = 0, hi = m.n - 1;
        while (lo < hi) {
            const int mid = (lo + hi + 1) / 2;
            if (m.cumS[mid] <= value) {
                lo = mid;
            } else {
                hi = mid - 1;
            }
        }
        const int s = lo;
        C -= step * m.cumS[s];
        R = (u32) (((u64) step * m.cumF[s]) >> 1);
        m.freq[s]++;
        m.total++;
        if (m.bits <= 14) {
            if (m.total == m.range) {
                const int sh = 15 - m.bits;
                u32 run = 0;
                for (int i = 0; i < m.n; i++) {
                    const u32 v = m.freq[i] << sh;
                    m.cumS[i] = run;
                    run += v;
                    m.cumF[i] = v;
                }
                m.bits++;
                m.range = 1u << m.bits;
            }
        } else if (m.total > 0x7FFF) {
            u32 run = 0, total = 0;
            for (int i = 0; i < m.n; i++) {
                u32 v = m.freq[i];
                m.cumF[i] = v;
                m.cumS[i] = run;
                run += v;
                if (v > 1) {
                    v >>= 1;
                    m.freq[i] = v;
                }
                total += v;
            }
            m.total = total;
        }
        return (u32) s;
    };
    size_t o = 0, scan = 0;
    while (o < unpacked) {
        const u32 s = sym(mainModel);
        u32 length;
        if (s > 0x3FF) {
            out[o++] = (u8) s;
            length = 1;
        } else {
            const u32 ctx = out[scan];
            const u32 slot = (s + dicCnt[ctx]) & 0x1FF;
            const int64_t src = dicPtr[ctx * 512 + slot];
            if (s > 0x1FF) {
                length = dicLen[ctx * 512 + slot];
            } else {
                const u32 t = sym(side);
                if (t > 2) {
                    length = t;
                } else if (t == 2) {
                    length = sym(side) << 8;
                    length |= sym(side);
                } else if (t == 1) {
                    length = sym(side) << 16;
                    length |= sym(side) << 8;
                    length |= sym(side);
                } else {
                    length = sym(side) << 24;
                    length |= sym(side) << 16;
                    length |= sym(side) << 8;
                    length |= sym(side);
                }
                length -= 1;
            }
            if (broken || length == 0 || length > 0x7FFFFFFF || src < 0 || o + length > unpacked) {
                *why = "a broken run at output " + std::to_string(o);
                return {};
            }
            for (u32 k = 0; k < length; k++) {  // may overlap: byte by byte
                out[o + k] = out[(size_t) src + k];
            }
            o += length;
        }
        if (broken) {
            *why = "the coded bytes end early";
            return {};
        }
        const size_t last = o - 1;
        if (scan < last) {
            const u32 ctx = out[scan];
            const u32 cnt = dicCnt[ctx];
            dicPtr[ctx * 512 + cnt] = (int64_t) scan + 1;
            dicLen[ctx * 512 + cnt] = length;
            dicCnt[ctx] = (cnt + 1) & 0x1FF;
            scan = last;
        }
    }
    return out;
}

// A .das file: yz2 from 0 (a raw stream) or from 0x400 (the archive's body); empty when the body is not packed.
std::vector<u8> unpackDas(const std::vector<u8>& d, std::string* why)
{
    u32 a, b;
    size_t s;
    const size_t base = yz2Header(d.data(), d.size(), 0, &a, &b, &s) ? 0 : 0x400;
    return yz2Decode(d, base, why);
}

std::vector<u8> readWhole(const std::string& path)
{
    std::vector<u8> d;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fail("cannot open %s (%s)", path.c_str(), strerror(errno));
    }
    fseeko(f, 0, SEEK_END);
    d.resize((size_t) ftello(f));
    fseeko(f, 0, SEEK_SET);
    if (!d.empty() && fread(d.data(), 1, d.size(), f) != d.size()) {
        fail("cannot read %s", path.c_str());
    }
    fclose(f);
    return d;
}

// --- The texture pack (textures.pak) from the recipe: each line names a texture
// image of a disc file by its ordinal - GameCube TPLs (magic 00 20 AF 30) found by offset, images in table order, a
// colour-indexed one only with a full palette in its TPL - with its GX format, size, mip levels the game hashes and the
// key the European disc gives. The key is the port's (gx_decode.cpp hashBytes / hashWords, built in from the port's
// source by puebloprep_build.sh), so the textures here are the ones the game asks the pack for. Per line: the image of that
// file with the same key, else the one of that ordinal with the same format and size (another edition's texels: its key
// computed from them). Then the textures made as the console would make them (texpak, pica_texture.cpp).
namespace texkey {
u32 rotl(u32 x, int r)
{
    return (x << r) | (x >> (32 - r));
}
u32 fmix(u32 h)
{
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}
#include "tex_hash.inc"
}  // namespace texkey

void tileShape(u32 format, u32* w, u32* h, u32* bytes)
{
    *bytes = 32;
    switch (format) {
    case 0: case 8: case 14: *w = 8; *h = 8; break;
    case 1: case 2: case 9: *w = 8; *h = 4; break;
    case 6: *w = 4; *h = 4; *bytes = 64; break;
    default: *w = 4; *h = 4; break;
    }
}

u32 textureBytes(u32 format, u32 width, u32 height, u32 levels)
{
    u32 tw, th, tb, total = 0;
    tileShape(format, &tw, &th, &tb);
    for (u32 l = 0; l < levels; l++) {
        total += ((width + tw - 1) / tw) * ((height + th - 1) / th) * tb;
        width = width > 1 ? width >> 1 : 1;
        height = height > 1 ? height >> 1 : 1;
    }
    return total;
}

struct TexImage {
    u32 data, format, width, height;
    u32 palAt, entries, palFormat;
    u32 minLod, maxLod, minFilter;  // the TPL image header's (--textures: the counts of levels; --scan-keys)
};

u32 be16(const u8* p)
{
    return (u32) p[0] << 8 | p[1];
}

// The images of a file in the recipe's order (TPLs found by their magic, images in table order).
std::vector<TexImage> texImages(const std::vector<u8>& d)
{
    std::vector<TexImage> out;
    for (size_t o = 0; o + 12 <= d.size(); o += 4) {
        if (be32(&d[o]) != 0x0020AF30u) {
            continue;
        }
        const u32 n = be32(&d[o + 4]), table = be32(&d[o + 8]);
        if (n == 0 || n > 256 || o + table + (u64) n * 8 > d.size()) {
            continue;
        }
        for (u32 i = 0; i < n; i++) {
            const u32 ih = be32(&d[o + table + i * 8]);
            if (o + ih + 0x24 > d.size()) {
                continue;
            }
            const u8* h = &d[o + ih];
            const u32 height = be16(h), width = be16(h + 2), format = be32(h + 4), data = be32(h + 8);
            if (format > 14 || format == 7 || (format > 10 && format != 14) || !width || !height || width > 1024 ||
                height > 1024) {
                continue;
            }
            TexImage im{(u32) (o + data), format, width, height, 0, 0, 0, h[0x21], h[0x22], be32(h + 0x14)};
            if (format >= 8 && format <= 10) {
                const u32 ph = be32(&d[o + table + i * 8 + 4]);
                const u32 want = format == 8 ? 16 : 256;
                if (format == 10 || !ph || o + ph + 12 > d.size() || be16(&d[o + ph]) != want ||
                    o + be32(&d[o + ph + 8]) + want * 2 > d.size()) {
                    continue;
                }
                im.entries = want;
                im.palFormat = be32(&d[o + ph + 4]) & 3;
                im.palAt = (u32) (o + be32(&d[o + ph + 8]));
            }
            if (o + data + textureBytes(format, width, height, 1) > d.size()) {
                continue;
            }
            out.push_back(im);
        }
    }
    return out;
}

// The key of an image hashed with `levels` mip levels (0: the levels run past the file's end).
u64 texKey(const std::vector<u8>& d, const TexImage& im, u32 levels)
{
    const u32 bytes = textureBytes(im.format, im.width, im.height, levels);
    if ((u64) im.data + bytes > d.size()) {
        return 0;
    }
    const u32 shape[4] = {im.format, im.width, im.height, levels};
    u64 key = texkey::hashBytes(&d[im.data], bytes, texkey::hashWords(shape, 4)) | 1;
    if (im.entries) {
        key ^= texkey::hashBytes(&d[im.palAt], im.entries * 2, im.palFormat) & ~1ull;
    }
    return key;
}

// The texture records (runtime/re4_tex_dump.h, RTX2: level 0 and the palette) into `dump`.
struct DumpHead {
    u32 magic, keyLo, keyHi, address;
    uint16_t format, width, height, tlutFormat;
    u32 dataBytes, entries;
};

void putRecord(FILE* f, const std::vector<u8>& d, const TexImage& im, u64 key)
{
    DumpHead h{};
    h.magic = 0x32585452u;
    h.keyLo = (u32) key;
    h.keyHi = (u32) (key >> 32);
    h.address = (u32) (key >> 7) & 0x7FFFFFE0u;  // distinct per texture (texpak leaves out an address of many keys)
    h.format = (uint16_t) im.format;
    h.width = (uint16_t) im.width;
    h.height = (uint16_t) im.height;
    h.tlutFormat = (uint16_t) im.palFormat;
    h.dataBytes = textureBytes(im.format, im.width, im.height, 1);
    h.entries = im.entries;
    static const u8 zero[4] = {};
    const u32 tail = (h.dataBytes + h.entries * 2) & 3;
    if (fwrite(&h, sizeof(h), 1, f) != 1 || fwrite(&d[im.data], 1, h.dataBytes, f) != h.dataBytes ||
        (h.entries && fwrite(&d[im.palAt], 2, h.entries, f) != h.entries) || (tail && fwrite(zero, 1, 4 - tail, f) != 4 - tail)) {
        fail("cannot write the texture records (%s)", strerror(errno));
    }
}

// The texture images of every file under `data` (plain, and a .das unpacked as yz2:PATH), in path order
template <class Fn>
void forEachFile(const std::string& data, Fn fn)
{
    std::vector<std::string> paths;
    for (const auto& e : fs::recursive_directory_iterator(fs::u8path(data))) {
        if (e.is_regular_file()) {
            paths.push_back(fs::relative(e.path(), fs::u8path(data)).generic_u8string());
        }
    }
    std::sort(paths.begin(), paths.end());
    for (size_t n = 0; n < paths.size(); n++) {
        const std::string& rel = paths[n];
        const std::vector<u8> raw = readWhole(data + "/" + rel);
        std::vector<TexImage> ims = texImages(raw);
        if (!ims.empty()) {
            fn(rel, raw, ims);
        }
        std::string why;
        const std::vector<u8> d = unpackDas(raw, &why);
        if (!d.empty() && !(ims = texImages(d)).empty()) {
            fn("yz2:" + rel, d, ims);
        }
        if ((n + 1) % 500 == 0) {
            say("textures: %3d %%  %s", (int) ((n + 1) * 100 / paths.size()), rel.c_str());
        }
    }
}

u32 chainLevels(const TexImage& im)
{
    u32 n = 1;
    for (u32 w = im.width, h = im.height; w > 1 || h > 1; w = w > 1 ? w >> 1 : 1, h = h > 1 ? h >> 1 : 1) {
        n++;
    }
    return n;
}

// (--scan-keys) every image with its keys for each count of mip levels that fits
void scanKeys(const std::string& data, const std::string& outPath)
{
    FILE* o = fopen(outPath.c_str(), "w");
    if (!o) {
        fail("cannot write %s (%s)", outPath.c_str(), strerror(errno));
    }
    u64 images = 0, keys = 0;
    forEachFile(data, [&](const std::string& name, const std::vector<u8>& d, const std::vector<TexImage>& ims) {
        for (size_t k = 0; k < ims.size(); k++) {
            const TexImage& im = ims[k];
            const u64 content = texKey(d, im, 1);
            if (!content) {
                continue;
            }
            images++;
            for (u32 l = 1; l <= chainLevels(im); l++) {
                const u64 key = l == 1 ? content : texKey(d, im, l);
                if (!key) {
                    break;
                }
                keys++;
                fprintf(o, "%s %zu %u %u %u %u %016llx %016llx %u %u %u %u\n", name.c_str(), k, im.format, im.width,
                        im.height, l, (unsigned long long) key, (unsigned long long) content,
                        textureBytes(im.format, im.width, im.height, 1), im.minLod, im.maxLod, im.minFilter);
            }
        }
    });
    if (fclose(o) != 0) {
        fail("cannot write %s (%s)", outPath.c_str(), strerror(errno));
    }
    say("scan: %llu images, %llu keys -> %s", (unsigned long long) images, (unsigned long long) keys, outPath.c_str());
}

// --- textures.pre (2026-11-24): the console reads a disc file's textures from the pack ahead as the game opens the file
// (gx_3ds.cpp, RE4_3DS_TEXPRELOAD). The order (texpre_order.inc, from stage97_prelist_rooms.py): each file's
// images as the European disc's room tour showed them first - by ordinal, levels, format and size, no keys; the keys
// here from this disc's texels, as the pack's. An image of another shape at that ordinal (another edition's): left out.
// The file: "RPL1", version 1, files, keys (u32 each, little-endian); {u32 FNV-1a 32 of the path below files/ in
// lower case, first, count} per file by the hash; u64 keys.
struct PreOrder {
    const char* rel;    // below files/, lower case (the console's runtime/dvd.cpp re4_dvd_open_fn)
    const char* name;   // the image's file as forEachFile names it
    const char* items;  // "ordinal.levels.format.width.height ..."
};
const PreOrder kPreOrder[] = {
#include "texpre_order.inc"
};
const size_t kPreRows = sizeof(kPreOrder) / sizeof(kPreOrder[0]);
std::vector<std::vector<u64>> gPreKeys;           // per row of kPreOrder
std::vector<std::pair<std::string, size_t>> gPreByName;  // (name, row) by name
u64 gPreOther;                                    // images left out: another shape there

void preStart()
{
    gPreKeys.assign(kPreRows, {});
    gPreByName.clear();
    for (size_t r = 0; r < kPreRows; r++) {
        gPreByName.emplace_back(kPreOrder[r].name, r);
    }
    std::sort(gPreByName.begin(), gPreByName.end());
    gPreOther = 0;
}

void preCollect(const std::string& name, const std::vector<u8>& d, const std::vector<TexImage>& ims)
{
    auto it = std::lower_bound(gPreByName.begin(), gPreByName.end(), std::make_pair(name, (size_t) 0));
    for (; it != gPreByName.end() && it->first == name; ++it) {
        std::vector<u64>& out = gPreKeys[it->second];
        const char* p = kPreOrder[it->second].items;
        unsigned o, l, f, w, h;
        int used = 0;
        while (sscanf(p, "%u.%u.%u.%u.%u%n", &o, &l, &f, &w, &h, &used) == 5) {
            p += used;
            if (o < ims.size() && ims[o].format == f && ims[o].width == w && ims[o].height == h) {
                const u64 key = texKey(d, ims[o], l);
                if (key) {
                    out.push_back(key);
                }
            } else {
                gPreOther++;
            }
        }
    }
}

u32 preHash(const char* s)
{
    u32 h = 0x811C9DC5u;
    for (; *s; s++) {
        h = (h ^ (u8) *s) * 0x01000193u;
    }
    return h;
}

void putLe(std::vector<u8>& b, u64 v, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        b.push_back((u8) (v >> (8 * i)));
    }
}

void preWrite(const std::string& path)
{
    // each file's keys in the rows' order, once; files by their hash (alike hashes: one list)
    std::vector<std::pair<u32, std::vector<u64>>> files;
    std::vector<std::string> rels;
    for (size_t r = 0; r < kPreRows; r++) {
        const u32 h = preHash(kPreOrder[r].rel);
        size_t at = 0;
        while (at < files.size() && files[at].first != h) {
            at++;
        }
        if (at == files.size()) {
            files.push_back({h, {}});
        }
        std::vector<u64>& ks = files[at].second;
        for (u64 k : gPreKeys[r]) {
            if (std::find(ks.begin(), ks.end(), k) == ks.end()) {
                ks.push_back(k);
            }
        }
    }
    files.erase(std::remove_if(files.begin(), files.end(), [](const auto& f) { return f.second.empty(); }), files.end());
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<u8> b;
    u64 keys = 0;
    for (const auto& f : files) {
        keys += f.second.size();
    }
    putLe(b, 0x314C5052u, 4);
    putLe(b, 1, 4);
    putLe(b, files.size(), 4);
    putLe(b, keys, 4);
    u64 first = 0;
    for (const auto& f : files) {
        putLe(b, f.first, 4);
        putLe(b, first, 4);
        putLe(b, f.second.size(), 4);
        first += f.second.size();
    }
    for (const auto& f : files) {
        for (u64 k : f.second) {
            putLe(b, k, 8);
        }
    }
    const std::string part = path + ".part";
    gTemps.push_back(part);
    FILE* o = fopen(part.c_str(), "wb");
    if (!o || fwrite(b.data(), 1, b.size(), o) != b.size() || fclose(o) != 0) {
        fail("cannot write %s (%s)", part.c_str(), strerror(errno));
    }
    remove(path.c_str());
    if (rename(part.c_str(), path.c_str()) != 0) {
        fail("cannot rename %s (%s)", part.c_str(), strerror(errno));
    }
    say("textures: %s - %zu files, %llu keys read ahead on the console (%llu images of another shape left out)",
        path.c_str(), files.size(), (unsigned long long) keys, (unsigned long long) gPreOther);
}

// (--texture-list) textures.pre alone, the pack as it is: only the order's files read (some 400, not the whole disc)
void makeTextureList(const std::string& data, const std::string& pueblo)
{
    preStart();
    std::vector<std::string> names;
    for (size_t r = 0; r < kPreRows; r++) {
        names.push_back(kPreOrder[r].name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    size_t read = 0;
    for (const std::string& name : names) {
        const bool yz2 = name.rfind("yz2:", 0) == 0;
        const std::string full = data + "/" + (yz2 ? name.substr(4) : name);
        u64 have;
        if (!fileSize(full, &have)) {
            continue;  // (another edition: no such file)
        }
        std::vector<u8> d = readWhole(full);
        if (yz2) {
            std::string why;
            d = unpackDas(d, &why);
        }
        if (d.empty()) {
            continue;
        }
        preCollect(name, d, texImages(d));
        if (++read % 100 == 0) {
            say("textures: the list, %zu of %zu files", read, names.size());
        }
    }
    u64 pak;
    if (!fileSize(pueblo + "/textures.pak", &pak)) {
        say("textures: no textures.pak in %s - the console reads the list only with the pack (choose the pack too)",
            pueblo.c_str());
    }
    preWrite(pueblo + "/textures.pre");
}

// (--textures) the records of every distinct image of the disc (its key of 1 level) and an alias (RTXA, texpak.cpp)
// for each other key the game may give it: 2 .. maxLOD + 1 levels (the TPL header's maxLOD bounds the levels the game
// hashes - all 9857 keys of all-8 so, 2026-11-16)
void putAlias(FILE* f, u64 key, u64 to)
{
    const u32 a[8] = {0x41585452u, (u32) key, (u32) (key >> 32), (u32) to, (u32) (to >> 32), 0, 0, 0};
    if (fwrite(a, sizeof(a), 1, f) != 1) {
        fail("cannot write the texture records (%s)", strerror(errno));
    }
}

void fullRecords(const std::string& data, FILE* dump)
{
    std::set<u64> written, aliased;
    u64 images = 0;
    forEachFile(data, [&](const std::string& name, const std::vector<u8>& d, const std::vector<TexImage>& ims) {
        preCollect(name, d, ims);  // (textures.pre)
        for (const TexImage& im : ims) {
            const u64 content = texKey(d, im, 1);
            if (!content) {
                continue;
            }
            images++;
            if (written.insert(content).second) {
                putRecord(dump, d, im, content);
            }
            const u32 top = std::min(chainLevels(im), im.maxLod + 1);
            for (u32 l = 2; l <= top; l++) {
                const u64 key = texKey(d, im, l);
                if (!key) {
                    break;
                }
                if (key != content && !written.count(key) && aliased.insert(key).second) {
                    putAlias(dump, key, content);
                }
            }
        }
    });
    say("textures: %llu images on the disc, %zu distinct, %zu more keys of their mip levels", (unsigned long long) images,
        written.size(), aliased.size());
}

// --- The port's memory card (runtime/memcard.cpp: 2 MB, Memory Card 251, the port's byte order) and .gci saves
// (CubeEvil4's runtime/gci.cpp, card_files.cpp; their card is this file in memory)
const u32 kCardBytes = 2u << 20, kCardSector = 0x2000;
std::vector<u8> gCard;
std::string gCardPath;
bool gCardDirty;

void cardSum(const u8* p, int bytes, u8* store)  // __CARDCheckSum, as memcard.cpp storeCheckSum
{
    uint16_t sum = 0, inv = 0;
    for (int i = 0; i < bytes; i += 2) {
        uint16_t w;
        memcpy(&w, p + i, 2);
        sum += w;
        inv += (uint16_t) ~w;
    }
    sum = sum == 0xFFFF ? 0 : sum;
    inv = inv == 0xFFFF ? 0 : inv;
    memcpy(store, &sum, 2);
    memcpy(store + 2, &inv, 2);
}

// A new card formatted and empty, as the port makes one (memcard.cpp formatNew: the serial from its fixed flash id and
// the time, two empty directories and FATs, 251 free blocks)
void cardFormat(u8* mem)
{
    static const u8 kFixedFlashId[12] = {'R', 'E', '4', 'P', 'O', 'R', 'T', 'C', 'A', 'R', 'D', 'A'};
    memset(mem, 0xFF, kCardBytes);
    memset(mem, 0xFF, 5 * kCardSector);
    u8* id = mem;
    const uint16_t encode = 0, deviceId = 0, sizeMbit = (uint16_t) (kCardBytes * 8 / (1024 * 1024));
    const u32 zero = 0;
    memcpy(id + 0x24, &encode, 2);
    memcpy(id + 20, &zero, 4);
    memcpy(id + 24, &zero, 4);
    // OSGetTime: ticks (40.5 MHz) since 2000-01-01
    int64_t time = ((int64_t) ::time(nullptr) - 946684800) * 40500000, rand = time;
    for (int i = 0; i < 12; i++) {
        rand = (rand * 1103515245 + 12345) >> 16;
        id[i] = (u8) (kFixedFlashId[i] + rand);
        rand = ((rand * 1103515245 + 12345) >> 16) & 0x7FFF;
    }
    memcpy(id + 28, &zero, 4);
    memcpy(id + 12, &time, sizeof(time));
    memcpy(id + 0x20, &deviceId, 2);
    memcpy(id + 0x22, &sizeMbit, 2);
    cardSum(id, 0x1FC, id + 0x1FC);
    for (int i = 0; i < 2; i++) {
        u8* dir = mem + (1 + i) * kCardSector;
        const int16_t code = (int16_t) i;
        memcpy(dir + 0x1FFA, &code, 2);
        cardSum(dir, 0x1FFC, dir + 0x1FFC);
    }
    for (int i = 0; i < 2; i++) {
        u8* fat = mem + (3 + i) * kCardSector;
        memset(fat, 0, kCardSector);
        const uint16_t head[3] = {(uint16_t) i, (uint16_t) (kCardBytes / kCardSector - 5), 4};
        memcpy(fat + 4, head, sizeof(head));
        cardSum(fat + 4, 0x1FFC, fat);
    }
}

void cardOpen(const std::string& path)
{
    gCardPath = path;
    u64 have = 0;
    if (fileSize(path, &have)) {
        if (have != kCardBytes) {
            fail("%s: %llu bytes, not a card of the port (2 MB)", path.c_str(), (unsigned long long) have);
        }
        gCard = readWhole(path);
        say("card: %s", path.c_str());
    } else {
        gCard.assign(kCardBytes, 0xFF);
        cardFormat(gCard.data());
        gCardDirty = true;
        say("card: %s is new (formatted, empty)", path.c_str());
    }
}

void cardSave()
{
    if (!gCardDirty) {
        return;
    }
    const fs::path p = fs::u8path(gCardPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }
    const std::string part = gCardPath + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f || fwrite(gCard.data(), 1, gCard.size(), f) != gCard.size() || fclose(f) != 0) {
        fail("cannot write %s (%s)", part.c_str(), strerror(errno));
    }
    std::error_code ec;
    fs::rename(fs::u8path(part), p, ec);
    if (ec) {
        fail("cannot rename %s (%s)", part.c_str(), ec.message().c_str());
    }
    gCardDirty = false;
    say("card: %s written", gCardPath.c_str());
}

void cardList()
{
    Re4CardFile files[127];
    const int n = re4_card_list(files, 127);
    if (n < 0) {
        fail("card: %s is not formatted (%d)", gCardPath.c_str(), n);
    }
    say("card: %d files, %d blocks free", n, re4_card_free_blocks());
    for (int i = 0; i < n; i++) {
        const Re4CardFile& f = files[i];
        if (f.save) {
            say("  %s%s %-12s %u blocks  chapter %u, room %03x, saved %d times, %u:%02u played, %04d-%02d-%02d %02d:%02d%s",
                f.game, f.company, f.name, f.blocks, f.chapter, f.room, (int) f.saveCount, f.playTime / 3600,
                f.playTime / 60 % 60, f.year, f.month, f.day, f.hour, f.minute, f.intact ? "" : "  (CRC broken)");
        } else {
            say("  %s%s %-12s %u blocks", f.game, f.company, f.name, f.blocks);
        }
    }
}

int cardImport(const std::string& path, bool replace)
{
    std::vector<u8> g = readWhole(path);
    if (g.size() >= 0x110 + 64 && memcmp(g.data(), "GCSAVE", 6) == 0) {  // .gcs (GameShark, copy tools): 0x110 bytes
        g.erase(g.begin(), g.begin() + 0x110);                           // of its own, then the .gci
        say("gci: %s: a .gcs - the .gci inside", path.c_str());
    }
    if (g.size() >= 4 && (memcmp(g.data(), "G4BP", 4) == 0 || memcmp(g.data(), "G4BJ", 4) == 0)) {  // the European
        const bool jp = g[3] == 'J';                                // or the Japanese game's: the port's id is G4BE
        memcpy(g.data(), "G4BE", 4);
        say("gci: %s: a file of the %s game (%s), taken as G4BE (the port's id)", path.c_str(), jp ? "Japanese" : "European",
            jp ? "G4BJ" : "G4BP");
    }
    if (!replace && g.size() >= 64 && memcmp(&g[8], "bh4_data", 8) == 0) {  // a save's slot taken: the first free one
        Re4CardFile files[127];                                                // of the game's 20 (bh4_data00..19)
        const int n = re4_card_list(files, 127);
        auto taken = [&](const char* name) {
            for (int i = 0; i < n; i++) {
                if (!strcmp(files[i].game, "G4BE") && !strcmp(files[i].name, name)) {
                    return true;
                }
            }
            return false;
        };
        char name[33] = {};
        memcpy(name, &g[8], 32);
        for (int slot = 0; n >= 0 && taken(name) && slot < 20; slot++) {
            char other[16];
            snprintf(other, sizeof(other), "bh4_data%02d", slot);
            if (!taken(other)) {
                memset(&g[8], 0, 32);
                memcpy(&g[8], other, 10);
                say("gci: %s: %s is on the card, written as %s", path.c_str(), name, other);
                break;
            }
        }
    }
    char note[256] = "";
    const int r = re4_gci_import(g.data(), (unsigned) g.size(), replace ? 1 : 0, note, sizeof(note));
    if (r != RE4_CARD_OK) {
        say("gci: %s: not imported - %s%s", path.c_str(), note[0] ? note : "error",
            r == RE4_CARD_EXISTS ? " (--replace to write over it)" : "");
        return 1;
    }
    say("gci: %s imported", path.c_str());
    return 0;
}

int cardExport(const std::string& dir)
{
    Re4CardFile files[127];
    const int n = re4_card_list(files, 127);
    if (n < 0) {
        fail("card: %s is not formatted (%d)", gCardPath.c_str(), n);
    }
    if (!makeDirs(dir)) {
        fail("cannot make %s (%s)", dir.c_str(), strerror(errno));
    }
    int problems = 0, done = 0;
    std::vector<u8> out(RE4_GCI_HEADER + 128 * RE4_CARD_BLOCK);
    for (int i = 0; i < n; i++) {
        char note[256] = "", name[96] = "";
        const int bytes = re4_gci_export(files[i].index, out.data(), (unsigned) out.size(), note, sizeof(note));
        if (bytes < 0 || re4_gci_file_name(files[i].index, name, sizeof(name)) != RE4_CARD_OK) {
            say("gci: %s: not exported - %s", files[i].name, note[0] ? note : "error");
            problems++;
            continue;
        }
        const std::string p = dir + "/" + name;
        FILE* f = fopen(p.c_str(), "wb");
        if (!f || fwrite(out.data(), 1, (size_t) bytes, f) != (size_t) bytes || fclose(f) != 0) {
            fail("cannot write %s (%s)", p.c_str(), strerror(errno));
        }
        say("gci: %s", p.c_str());
        done++;
    }
    say("gci: %d of %d files exported", done, n);
    return problems ? 1 : 0;
}

}  // namespace

// (CubeEvil4's card functions call these: the card is gCard)
extern "C" unsigned char* re4_memcard_mem(void)
{
    return gCard.empty() ? nullptr : gCard.data();
}
extern "C" void re4_memcard_store(unsigned, unsigned)
{
    gCardDirty = true;
}
extern "C" void re4_memcard_flush(int)
{
}
extern "C" void re4_log(const char* fmt, ...)
{
    if (!gLog) {
        return;
    }
    va_list a;
    va_start(a, fmt);
    vfprintf(gLog, fmt, a);
    va_end(a);
    fputc('\n', gLog);
}

namespace {
}  // namespace
extern "C++" int texpak_main(int argc, char** argv);  // the port's texpak (3ds/tools/texpak.cpp, its main renamed)
namespace {

void makeTextures(const std::string& recipePath, const std::string& data, const std::string& pueblo)
{
    FILE* r = recipePath.empty() ? nullptr : fopen(recipePath.c_str(), "r");
    if (!r && !recipePath.empty()) {
        fail("cannot open the recipe %s (%s)", recipePath.c_str(), strerror(errno));
    }
    struct Row {
        std::string path;
        u32 ordinal, format, width, height, levels;
        u64 key;
    };
    std::vector<Row> rows;
    char line[1024];
    while (r && fgets(line, sizeof(line), r)) {
        if (line[0] == '#') {
            continue;
        }
        char path[800];
        Row w;
        unsigned long long key;
        if (sscanf(line, "%799s %u %u %u %u %u %llx", path, &w.ordinal, &w.format, &w.width, &w.height, &w.levels,
                   &key) != 7) {
            fail("the recipe %s: a broken line: %s", recipePath.c_str(), line);
        }
        w.path = path;
        w.key = key;
        rows.push_back(std::move(w));
    }
    if (r) {
        fclose(r);
        say("textures: %zu in the recipe", rows.size());
    }
    const std::string dumpPath = pueblo + "/textures.records.part";
    gTemps.push_back(dumpPath);
    gTemps.push_back(pueblo + "/textures.pak.part");
    FILE* dump = fopen(dumpPath.c_str(), "wb");
    if (!dump) {
        fail("cannot write %s (%s)", dumpPath.c_str(), strerror(errno));
    }
    if (recipePath.empty()) {  // (--textures: every image of the disc)
        preStart();
        fullRecords(data, dump);
        rows.clear();
    }
    u32 same = 0, other = 0, none = 0, files = 0;
    std::string current;
    std::vector<u8> d;
    std::vector<TexImage> ims;
    for (size_t i = 0; i < rows.size(); i++) {
        const Row& w = rows[i];
        if (w.path != current) {  // (the recipe is sorted by path)
            current = w.path;
            files++;
            const bool yz2 = current.rfind("yz2:", 0) == 0;
            const std::string full = data + "/" + (yz2 ? current.substr(4) : current);
            u64 have;
            d.clear();
            ims.clear();
            if (fileSize(full, &have)) {
                d = readWhole(full);
                if (yz2) {
                    std::string why;
                    d = unpackDas(d, &why);
                    if (d.empty()) {
                        say("warning: %s: %s", full.c_str(), why.c_str());
                    }
                }
                ims = texImages(d);
            }
            if (files % 100 == 0) {
                say("textures: %3d %%  %s", (int) (i * 100 / rows.size()), current.c_str());
            }
        }
        const TexImage* found = nullptr;
        u64 key = 0;
        if (w.ordinal < ims.size()) {  // the image of that ordinal: the same texels?
            const TexImage& im = ims[w.ordinal];
            if (im.format == w.format && im.width == w.width && im.height == w.height &&
                texKey(d, im, w.levels) == w.key) {
                found = &im;
                key = w.key;
            }
        }
        for (size_t k = 0; !found && k < ims.size(); k++) {  // elsewhere in the file
            const TexImage& im = ims[k];
            if (im.format == w.format && im.width == w.width && im.height == w.height &&
                texKey(d, im, w.levels) == w.key) {
                found = &im;
                key = w.key;
            }
        }
        if (found) {
            same++;
        } else if (w.ordinal < ims.size() && ims[w.ordinal].format == w.format && ims[w.ordinal].width == w.width &&
                   ims[w.ordinal].height == w.height && (key = texKey(d, ims[w.ordinal], w.levels)) != 0) {
            found = &ims[w.ordinal];  // another edition's texels in that place
            other++;
        } else {
            none++;
            continue;
        }
        putRecord(dump, d, *found, key);
    }
    if (fclose(dump) != 0) {
        fail("cannot write %s (%s)", dumpPath.c_str(), strerror(errno));
    }
    if (!recipePath.empty()) {
        say("textures: %u as on the European disc, %u of this disc's own, %u not found (%u files read)", same, other,
            none, files);
    }
    const std::string pak = pueblo + "/textures.pak";
    const std::string pakPart = pak + ".part";
    std::vector<std::string> args = {"texpak", pakPart, dumpPath};
    std::vector<char*> argv;
    for (std::string& a : args) {
        argv.push_back(&a[0]);
    }
    argv.push_back(nullptr);
    if (texpak_main((int) args.size(), argv.data()) != 0) {
        fail("the texture pack was not made");
    }
    remove(dumpPath.c_str());
    remove(pak.c_str());
    if (rename(pakPart.c_str(), pak.c_str()) != 0) {
        fail("cannot rename %s (%s)", pakPart.c_str(), strerror(errno));
    }
    say("textures: %s", pak.c_str());
    if (recipePath.empty()) {
        preWrite(pueblo + "/textures.pre");
    }
}

// --- The movies (Movie/*.sfd: Sofdec, MPEG-1 512x336 at 59.94 + ADX 32 kHz) for the New 3DS's H.264 decoder (MVD),
// as 3ds/movie_3ds.cpp reads them: movie/NAME.mvd - "RE4MVD01", u16
// width, u16 height, u32 fps numerator, denominator, frames, records, 0 (little-endian), then records of u32 length +
// a NAL unit with its 00 00 01 prefix (no SEI / delimiter / filler) - and movie/NAME.pcm (s16le stereo 32 000 Hz).
// Through an ffmpeg with libx264 (GPL: a separate program next to PuebloPrep, not built in).
const char* const kMovies[] = {"adaend_c", "adaend_m", "demo0", "demo1", "ending", "opening", "r214_ev", "r229_ev",
                               "r333_ev"};

std::string shellQuoted(const std::string& s)
{
    std::string q = "\"";
    for (char c : s) {
#ifdef _WIN32
        if (c == '"') {  // (no quote is allowed in a Windows path; backslashes are its separators)
            continue;
        }
#else
        if (c == '"' || c == '\\' || c == '$' || c == '`') {
            q += '\\';
        }
#endif
        q += c;
    }
    return q + "\"";
}

bool run(const std::string& cmd)
{
#ifdef _WIN32
    // cmd.exe /c strips one pair of quotes around the whole line; started without a console window
    std::string line = "cmd.exe /c \"" + cmd + "\"";
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
#else
    return std::system(cmd.c_str()) == 0;
#endif
}

}  // namespace

// Whether `ffmpeg` runs and has libx264: 0 no ffmpeg there, 1 an ffmpeg without libx264, 2 one with it (one black frame
// encoded by libx264; "-h encoder=libx264" exits 0 for any encoder name). The window calls it too (puebloprep_gui.cpp).
extern "C" int puebloprep_ffmpeg_check(const char* ffmpeg)
{
    const std::string f = shellQuoted(ffmpeg);
    if (!run(f + " -hide_banner -version > " + kNullDevice + " 2>&1")) {
        return 0;
    }
    if (!run(f + " -hide_banner -v error -f lavfi -i color=c=black:s=64x64:d=0.1 -frames:v 1 -c:v libx264 -f null - > " +
             kNullDevice + " 2>&1")) {
        return 1;
    }
    return 2;
}

namespace {

void makeMovies(const std::string& data, const std::string& pueblo, const std::string& ffmpeg)
{
    const std::string dir = pueblo + "/movie";
    if (!makeDirs(dir)) {
        fail("cannot make %s (%s)", dir.c_str(), strerror(errno));
    }
    if (const int found = puebloprep_ffmpeg_check(ffmpeg.c_str()); found < 2) {
        fail(found ? "this ffmpeg (%s) has no libx264 encoder: take a full build (README, \"ffmpeg\")"
                   : "no ffmpeg (%s): put ffmpeg next to PuebloPrep or give --ffmpeg PATH (README, \"ffmpeg\")",
             ffmpeg.c_str());
    }
    const u32 W = 512, H = 256, fpsNum = 30000, fpsDen = 1001;
    int made = 0;
    for (const char* name : kMovies) {
        std::string src;
        for (const char* disc : {"disc0", "disc1"}) {
            const std::string p = data + "/" + disc + "/files/Movie/" + name + ".sfd";
            u64 have;
            if (src.empty() && fileSize(p, &have)) {
                src = p;
            }
        }
        if (src.empty()) {
            say("warning: no Movie/%s.sfd on the discs", name);
            continue;
        }
        const std::string mvd = dir + "/" + name + ".mvd", pcm = dir + "/" + name + ".pcm";
        u64 mvdBytes = 0, pcmBytes = 0;
        if (fileSize(mvd, &mvdBytes) && mvdBytes > 32 && fileSize(pcm, &pcmBytes) && pcmBytes > 0) {
            made++;
            continue;  // (from an earlier run)
        }
        say("movies: %s", name);
        const std::string h264 = dir + "/" + name + ".h264.part";
        char vf[128];
        snprintf(vf, sizeof(vf), "fps=%u/%u,scale=%u:%u:flags=lanczos,format=yuv420p", fpsNum, fpsDen, W, H);
        if (!run(shellQuoted(ffmpeg) + " -hide_banner -v error -y -i " + shellQuoted(src) + " -an -vf " + vf +
                 " -c:v libx264 -preset slow -profile:v baseline -level 2.1 -crf 19 -maxrate 2500k -bufsize 5000k"
                 " -x264-params ref=2:keyint=60:min-keyint=15:slices=1:aud=0:annexb=1 -f h264 " + shellQuoted(h264))) {
            fail("ffmpeg could not make the picture of %s", name);
        }
        const std::vector<u8> s = readWhole(h264);
        std::vector<size_t> starts;
        for (size_t i = 0; i + 3 <= s.size(); i++) {
            if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
                starts.push_back(i + 3);
                i += 2;
            }
        }
        std::vector<u8> recs;
        u32 count = 0, frames = 0;
        for (size_t k = 0; k < starts.size(); k++) {
            const size_t b = starts[k];
            size_t e = k + 1 < starts.size() ? starts[k + 1] - 3 : s.size();
            while (e > b && s[e - 1] == 0) {  // the zero of a 4-byte prefix that follows
                e--;
            }
            if (e <= b) {
                continue;
            }
            const u32 type = s[b] & 0x1F;
            if (type == 6 || type == 9 || type == 12) {
                continue;
            }
            const u32 len = (u32) (e - b) + 3;
            const u8 lenBytes[4] = {(u8) len, (u8) (len >> 8), (u8) (len >> 16), (u8) (len >> 24)};
            recs.insert(recs.end(), lenBytes, lenBytes + 4);
            recs.push_back(0);
            recs.push_back(0);
            recs.push_back(1);
            recs.insert(recs.end(), s.begin() + b, s.begin() + e);
            count++;
            frames += type == 1 || type == 5;
        }
        std::vector<u8> head(32, 0);
        memcpy(head.data(), "RE4MVD01", 8);
        auto le16 = [&](size_t at, u32 v) { head[at] = (u8) v; head[at + 1] = (u8) (v >> 8); };
        auto le32 = [&](size_t at, u32 v) { for (int i = 0; i < 4; i++) head[at + i] = (u8) (v >> (8 * i)); };
        le16(8, W);
        le16(10, H);
        le32(12, fpsNum);
        le32(16, fpsDen);
        le32(20, frames);
        le32(24, count);
        head.insert(head.end(), recs.begin(), recs.end());
        writeBytes(mvd + ".part", head);
        remove(h264.c_str());
        if (!run(shellQuoted(ffmpeg) + " -hide_banner -v error -y -i " + shellQuoted(src) +
                 " -vn -ac 2 -ar 32000 -f s16le -c:a pcm_s16le " + shellQuoted(pcm + ".part"))) {
            fail("ffmpeg could not make the sound of %s", name);
        }
        remove(mvd.c_str());
        remove(pcm.c_str());
        if (rename((mvd + ".part").c_str(), mvd.c_str()) != 0 || rename((pcm + ".part").c_str(), pcm.c_str()) != 0) {
            fail("cannot rename the files of %s (%s)", name, strerror(errno));
        }
        made++;
        say("movies: %s: %u frames, %.1f MB", name, frames, head.size() / 1048576.0);
    }
    say("movies: %d of %zu in %s", made, sizeof(kMovies) / sizeof(kMovies[0]), dir.c_str());
}

void usage()
{
    printf("%s\n"
           "  puebloprep-cli --in DISC1.iso --in DISC2.iso [--out DIR]      unpack to DIR/3ds/pueblo/data (DIR: PuebloCTR-SD)\n"
           "  puebloprep-cli --in DISC1.iso --in DISC2.iso --check DIR      compare a prepared folder with the images\n"
           "  puebloprep-cli --in DISC.iso --info                           what the image is\n"
           "  ... --textures                                         also textures.pak and textures.pre (without --in: from the folder)\n"
           "  ... --texture-list                                     textures.pre only (the list of textures read ahead; under a minute)\n"
           "  ... --movies [--ffmpeg PATH]                           also the movies (ffmpeg with libx264)\n"
           "  puebloprep-cli [--out DIR | --card CARD] --import-gci F.gci [--replace]  saves onto the card (DIR/3ds/pueblo/saves/memcard.bin)\n"
           "  puebloprep-cli [--out DIR | --card CARD] --export-gci DIR2 | --list-card the card's saves as .gci / what it holds\n"
           "Then copy the folder 3ds from DIR to the root of your SD card. Your own discs only.\n",
           kVersion);
}

}  // namespace

extern "C" int puebloprep_run(int argc, char** argv);
extern "C" void puebloprep_set_window(void (*windowSay)(const char*))
{
    gWindowSay = windowSay;
}

#ifndef PUEBLOPREP_WINDOW
int main(int argc, char** argv)
{
    return puebloprep_run(argc, argv);
}
#endif

// The console part (main; the window, puebloprep_gui.cpp, calls it on a worker thread)
extern "C" int puebloprep_run(int argc, char** argv)
{
#ifdef _WIN32
    _fmode = _O_BINARY;  // (the RVZ reader's open() has no O_BINARY)
#endif
    std::vector<std::string> ins;
    std::string out = "PuebloCTR-SD", checkDir, recipe, ffmpeg = "ffmpeg";
    bool info = false, movies = false, textures = false, textureList = false;
    std::string card, exportGci;  // (R5: saves)
    std::vector<std::string> gcis;
    bool replace = false, listCard = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--in" && i + 1 < argc) {
            ins.push_back(argv[++i]);
        } else if (a == "--out" && i + 1 < argc) {
            out = argv[++i];
        } else if (a == "--check" && i + 1 < argc) {
            checkDir = argv[++i];
        } else if (a == "--card" && i + 1 < argc) {
            card = argv[++i];
        } else if (a == "--import-gci" && i + 1 < argc) {
            gcis.push_back(argv[++i]);
        } else if (a == "--export-gci" && i + 1 < argc) {
            exportGci = argv[++i];
        } else if (a == "--list-card") {
            listCard = true;
        } else if (a == "--replace") {
            replace = true;
        } else if (a == "--textures") {
            textures = true;
        } else if (a == "--texture-list") {  // (textures.pre alone; --textures makes it too)
            textureList = true;
        } else if (a == "--recipe" && i + 1 < argc) {  // (the textures of a recipe only: work/data/texture-recipe-v2.txt)
            recipe = argv[++i];
            textures = true;
        } else if (a == "--scan-keys" && i + 2 < argc) {  // (R5: the keys of every image, for measures)
            scanKeys(argv[i + 1], argv[i + 2]);
            return 0;
        } else if (a == "--movies") {
            movies = true;
        } else if (a == "--ffmpeg" && i + 1 < argc) {
            ffmpeg = argv[++i];
        } else if (a == "--info") {
            info = true;
        } else if (a == "--yz2" && i + 2 < argc) {  // (a check of the decoder: one .das file decompressed)
            std::string why;
            const std::vector<u8> body = unpackDas(readWhole(argv[i + 1]), &why);
            if (body.empty()) {
                fprintf(stderr, "%s: %s\n", argv[i + 1], why.c_str());
                return 1;
            }
            FILE* o = fopen(argv[i + 2], "wb");
            if (!o || fwrite(body.data(), 1, body.size(), o) != body.size() || fclose(o) != 0) {
                return 1;
            }
            return 0;
        } else {
            usage();
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (!gcis.empty() || !exportGci.empty() || listCard) {  // (R5) the card only
        gLog = fopen("puebloprep.log", "a");
        say("%s", kVersion);
        cardOpen(!card.empty() ? card : out + "/3ds/pueblo/saves/memcard.bin");
        int problems = 0;
        for (const std::string& g : gcis) {
            problems += cardImport(g, replace);
        }
        cardSave();
        if (!exportGci.empty()) {
            problems += cardExport(exportGci);
        }
        if (listCard || !gcis.empty()) {
            cardList();
        }
        return problems ? 1 : 0;
    }
    if ((ins.empty() && ((!textures && !textureList && !movies) || out.empty())) ||
        (out.empty() && checkDir.empty() && !info)) {
        usage();
        return 2;
    }
    gLog = fopen("puebloprep.log", "a");
    say("%s", kVersion);
    // RVZ (Dolphin's compressed images): unpacked to a plain image first, removed at the end (also after an error)
    atexit(removeTemps);
    for (size_t k = 0; k < ins.size(); k++) {
        if (!re4::IsRvz(ins[k].c_str())) {
            continue;
        }
        re4::RvzInfo ri;
        std::string why;
        if (!re4::RvzInfoOf(ins[k].c_str(), &ri, &why)) {
            fail("%s: %s", ins[k].c_str(), why.c_str());
        }
        // the system's temporary folder when it has room (not the SD card), else next to the output
        std::error_code ec;
        std::string dir = fs::temp_directory_path(ec).u8string();
        if (ec || freeBytes(dir) < ri.discSize + (64u << 20)) {
            dir = !checkDir.empty() ? checkDir : out;
            makeDirs(dir);
        }
        const std::string tmp = dir + "/puebloprep-disc" + std::to_string(k) + ".iso.part";
        gTemps.push_back(tmp);
        say("%s: RVZ of %.0f MB, unpacking to %s", ins[k].c_str(), ri.discSize / 1048576.0, tmp.c_str());
        int shown = -1;
        if (!re4::RvzToIso(ins[k].c_str(), tmp.c_str(),
                           [&](u64 done, u64 total) {
                               const int pc = (int) (done * 100 / (total ? total : 1));
                               if (pc / 10 != shown / 10) {
                                   shown = pc;
                                   say("RVZ: %3d %%", pc);
                               }
                               return true;
                           },
                           &why)) {
            remove(tmp.c_str());
            fail("%s: %s", ins[k].c_str(), why.c_str());
        }
        ins[k] = tmp;
    }
    if (ins.empty()) {  // the textures and / or the movies only, from a card already unpacked
        std::error_code noData;
        if (!fs::is_directory(native(out + "/3ds/pueblo/data/disc0/files"), noData)) {
            fail("%s holds no game data yet - choose the disc images (--in) first", out.c_str());
        }
        if (textures) {
            makeTextures(recipe, out + "/3ds/pueblo/data", out + "/3ds/pueblo");
        } else if (textureList) {
            makeTextureList(out + "/3ds/pueblo/data", out + "/3ds/pueblo");
        }
        if (movies) {
            makeMovies(out + "/3ds/pueblo/data", out + "/3ds/pueblo", ffmpeg);
        }
        return 0;
    }
    std::vector<Image> ims;
    for (const std::string& p : ins) {
        ims.push_back(openImage(p));
        describe(ims.back());
    }
    if (info) {
        return 0;
    }
    checkPair(ims);
    const std::string root = !checkDir.empty() ? checkDir : out;
    const std::string data = root + "/3ds/pueblo/data";
    if (!checkDir.empty()) {
        int problems = 0;
        for (const Image& im : ims) {
            problems += check(im, data);
        }
        say(problems ? "the folder differs from the discs" : "the folder matches the discs");
        return problems ? 1 : 0;
    }
    u64 need = 0;
    for (const Image& im : ims) {
        need += im.fileBytes + (8u << 20);
    }
    need += textures ? 400ull << 20 : 0;  // textures.pak (PAL 2026-11-16: ~380 MB)
    need += movies ? 220ull << 20 : 0;         // movie/
    if (!makeDirs(data)) {
        fail("cannot make %s (%s)", data.c_str(), strerror(errno));
    }
    if (const u64 room = freeBytes(data)) {
        say("%.0f MB to write, %.0f MB free on %s", need / 1048576.0, room / 1048576.0, root.c_str());
        if (room < need / 2) {  // (files already there are skipped: a rough check only)
            fail("not enough room on %s", root.c_str());
        }
    }
    for (const Image& im : ims) {
        unpack(im, data);
    }
    if (textures) {
        makeTextures(recipe, data, root + "/3ds/pueblo");
    } else if (textureList) {
        makeTextureList(data, root + "/3ds/pueblo");
    }
    if (movies) {
        makeMovies(data, root + "/3ds/pueblo", ffmpeg);
    }
    say("done: copy the folder %s/3ds to the root of your SD card (merge with the 3ds folder there)", root.c_str());
    return 0;
}
