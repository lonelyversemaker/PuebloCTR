// PuebloPrep's window (R5): the same work as the console part (puebloprep.cpp, compiled with -DPUEBLOPREP_WINDOW), run on a
// worker thread with the choices made here. Dear ImGui on SDL3 (its renderer and its native file dialogs). English
// only. The window never writes to the SD card: it makes a folder on the computer, and says what to copy where.
//
// Built by CMake (CMakeLists.txt here; the repository's CI for Windows / macOS / Linux).
// MIT licence (the repository's LICENSE).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // (main as WinMain on Windows)

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" int puebloprep_run(int argc, char** argv);                      // puebloprep.cpp
extern "C" void puebloprep_set_window(void (*windowSay)(const char*));     // puebloprep.cpp
extern "C" int puebloprep_ffmpeg_check(const char* ffmpeg);                 // puebloprep.cpp: 0 none, 1 no libx264, 2 ok

namespace {

namespace fs = std::filesystem;

// --- the log of the work (from the worker thread) and the dialogs' answers (from SDL's thread) ---
std::mutex gLock;
std::vector<std::string> gLines;
std::string gProgress;  // the last "NN %" line
std::atomic<bool> gBusy{false};
std::atomic<int> gResult{-1};  // -1 none yet, 0 done, 1 failed
std::string gFailure;

void windowSay(const char* line)
{
    std::lock_guard<std::mutex> l(gLock);
    if (strstr(line, " %  ")) {  // a progress line ("textures:  40 %  St2/...") - one place, not the log
        gProgress = line;
        return;
    }
    gLines.emplace_back(line);
}

struct Choice {
    std::string disc1, disc2, out, ffmpeg;
    int textures = 2;  // 2: the pack and its list, 1: the list only (textures.pre), 0: none
    bool movies = true;
    std::vector<std::string> saves;
};
Choice gChoice;

// SDL's dialogs answer on their own thread: the answer is kept here until the next frame takes it
struct Answer {
    int target = 0;  // 1 disc 1, 2 disc 2, 3 out, 4 ffmpeg, 5 saves
    std::vector<std::string> paths;
};
std::vector<Answer> gAnswers;

void dialogDone(void* target, const char* const* list, int)
{
    if (!list || !*list) {
        return;  // an error or cancelled
    }
    Answer a;
    a.target = (int) (intptr_t) target;
    for (; *list; list++) {
        a.paths.emplace_back(*list);
    }
    std::lock_guard<std::mutex> l(gLock);
    gAnswers.push_back(std::move(a));
}

const SDL_DialogFileFilter kImages[] = {{"GameCube disc images (ISO, GCM, RVZ)", "iso;gcm;rvz"}, {"All files", "*"}};
const SDL_DialogFileFilter kSaves[] = {{"GameCube saves (GCI, GCS)", "gci;gcs"}, {"All files", "*"}};
#ifdef _WIN32
const SDL_DialogFileFilter kPrograms[] = {{"ffmpeg.exe", "exe"}, {"All files", "*"}};
#else
const SDL_DialogFileFilter kPrograms[] = {{"All files", "*"}};
#endif

std::string folderOf(const std::string& path)
{
    if (path.empty()) {
        return {};
    }
    return fs::u8path(path).parent_path().u8string();
}

// --- the work, on a worker thread: PuebloPrep's own arguments ---
int runArgs(std::vector<std::string> args)
{
    args.insert(args.begin(), "puebloprep");
    std::vector<char*> argv;
    for (std::string& a : args) {
        argv.push_back(&a[0]);
    }
    argv.push_back(nullptr);
    try {
        return puebloprep_run((int) args.size(), argv.data());
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> l(gLock);
        gFailure = e.what();
        return 1;
    }
}

std::string ffmpegIn(const std::string& path);  // (below: a folder given for ffmpeg)

void work(Choice c)
{
    int r = 0;
    std::vector<std::string> args;
    if (!c.disc1.empty()) {
        args.insert(args.end(), {"--in", c.disc1});
    }
    if (!c.disc2.empty()) {
        args.insert(args.end(), {"--in", c.disc2});
    }
    args.insert(args.end(), {"--out", c.out});
    if (c.textures) {
        args.push_back(c.textures == 2 ? "--textures" : "--texture-list");
    }
    if (c.movies) {
        args.insert(args.end(), {"--movies", "--ffmpeg", ffmpegIn(c.ffmpeg)});
    }
    if (!c.disc1.empty() || !c.disc2.empty() || c.textures || c.movies) {
        r = runArgs(args);
    }
    if (r == 0 && !c.saves.empty()) {
        std::vector<std::string> s = {"--out", c.out};
        for (const std::string& g : c.saves) {
            s.insert(s.end(), {"--import-gci", g});
        }
        r = runArgs(s);
    }
    gResult = r == 0 ? 0 : 1;
    gBusy = false;
}

// --- the window ---
bool pathRow(const char* label, std::string* value, const char* id)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", value->c_str());
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-ImGui::CalcTextSize("Browse...").x - ImGui::GetStyle().ItemSpacing.x * 2 -
                            ImGui::GetStyle().FramePadding.x * 2);
    const bool edited = ImGui::InputText(id, buf, sizeof(buf));
    if (edited) {
        *value = buf;
    }
    ImGui::SameLine();
    ImGui::PushID(id);
    const bool browse = ImGui::Button("Browse...");
    ImGui::PopID();
    return browse;
}

// The output by default: a new folder PuebloCTR-SD next to the program (user's decision 2026-11-16); for a macOS
// .app next to the .app, not inside it. Without the program's folder - Documents.
std::string defaultOut()
{
    fs::path dir;
    if (const char* base = SDL_GetBasePath()) {
        dir = fs::u8path(base);
        const std::string b = dir.u8string();
        const size_t app = b.find(".app/Contents/");
        if (app != std::string::npos) {
            dir = fs::u8path(b.substr(0, app + 4)).parent_path();
        }
    } else if (const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) {
        dir = fs::u8path(docs);
    }
    return (dir / "PuebloCTR-SD").lexically_normal().u8string();
}

#ifdef _WIN32
const char* const kFfmpegName = "ffmpeg.exe";
#else
const char* const kFfmpegName = "ffmpeg";
#endif

// ffmpeg where a user puts it: next to the program, next to the .app (macOS: the base path is inside it), Homebrew's and
// the system's folders (from Finder the PATH has no Homebrew), C:\ffmpeg\bin; else the name alone (the PATH)
std::string defaultFfmpeg()
{
    std::vector<fs::path> at;
    if (const char* base = SDL_GetBasePath()) {
        const std::string b = fs::u8path(base).u8string();
        at.push_back(fs::u8path(b) / kFfmpegName);
        const size_t app = b.find(".app/Contents/");
        if (app != std::string::npos) {
            at.push_back(fs::u8path(b.substr(0, app + 4)).parent_path() / kFfmpegName);
        }
    }
#ifdef _WIN32
    at.push_back("C:\\ffmpeg\\bin\\ffmpeg.exe");
#else
    for (const char* p : {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"}) {
        at.push_back(p);
    }
#endif
    for (const fs::path& p : at) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) {
            return p.u8string();
        }
    }
    return "ffmpeg";  // from the PATH
}

// A folder given for ffmpeg: its ffmpeg, or bin/ffmpeg (an unpacked Windows build)
std::string ffmpegIn(const std::string& path)
{
    std::error_code ec;
    const fs::path p = fs::u8path(path);
    if (!path.empty() && fs::is_directory(p, ec)) {
        for (const fs::path& f : {p / kFfmpegName, p / "bin" / kFfmpegName}) {
            if (fs::is_regular_file(f, ec)) {
                return f.u8string();
            }
        }
    }
    return path;
}

// The ffmpeg's state, checked on a thread whenever the path changes: -1 checking, else puebloprep_ffmpeg_check's
std::atomic<int> gFfState{-1};
std::atomic<bool> gFfChecking{false};
std::string gFfChecked = "\x01";  // the path last checked (none yet)

void ffmpegCheck(std::string path)
{
    const int s = puebloprep_ffmpeg_check(ffmpegIn(path).c_str());
    gFfState = s;
    gFfChecking = false;
}

void ffmpegStatus()
{
    if (!gFfChecking && gChoice.ffmpeg != gFfChecked) {
        gFfChecked = gChoice.ffmpeg;
        gFfState = -1;
        gFfChecking = true;
        std::thread(ffmpegCheck, gChoice.ffmpeg).detach();
    }
    const bool path = gChoice.ffmpeg == "ffmpeg" || gChoice.ffmpeg == "ffmpeg.exe";
    switch (gFfChecking || gChoice.ffmpeg != gFfChecked ? -1 : (int) gFfState) {
    case -1:
        ImGui::TextDisabled("Checking ffmpeg...");
        break;
    case 2:
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "ffmpeg works, with libx264%s: the movies can be made.",
                           path ? " (found on the PATH)" : "");
        break;
    case 1:
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "This ffmpeg has no libx264 encoder. Take a full build "
                           "(README, \"ffmpeg\") and choose it with Browse..., or untick Movies.");
        break;
    default:
        ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "ffmpeg not found. Install it (README, \"ffmpeg\") and choose "
                           "%s with Browse..., or untick Movies.", kFfmpegName);
        break;
    }
}

bool ffmpegReady()
{
    return !gFfChecking && gChoice.ffmpeg == gFfChecked && gFfState == 2;
}

void frame(SDL_Window* window)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("PuebloPrep", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    {
        std::lock_guard<std::mutex> l(gLock);
        for (Answer& a : gAnswers) {
            switch (a.target) {
            case 1: gChoice.disc1 = a.paths[0]; break;
            case 2: gChoice.disc2 = a.paths[0]; break;
            case 3: gChoice.out = a.paths[0]; break;
            case 4: gChoice.ffmpeg = a.paths[0]; break;
            case 5: gChoice.saves.insert(gChoice.saves.end(), a.paths.begin(), a.paths.end()); break;
            }
        }
        gAnswers.clear();
    }
    const bool busy = gBusy;
    ImGui::TextUnformatted("PuebloCTR - prepares your own GameCube discs of the game for the New Nintendo 3DS.");
    ImGui::TextDisabled("European (G4BP08), US (G4BE08) or Japanese (G4BJ08) release, both discs. Nothing is written to the SD card.");
    ImGui::Separator();
    ImGui::BeginDisabled(busy);
    if (pathRow("Disc 1 image", &gChoice.disc1, "##disc1")) {
        SDL_ShowOpenFileDialog(dialogDone, (void*) 1, window, kImages, 2, gChoice.disc1.empty() ? nullptr : gChoice.disc1.c_str(), false);
    }
    if (pathRow("Disc 2 image", &gChoice.disc2, "##disc2")) {
        const std::string at = gChoice.disc2.empty() ? folderOf(gChoice.disc1) : gChoice.disc2;
        SDL_ShowOpenFileDialog(dialogDone, (void*) 2, window, kImages, 2, at.empty() ? nullptr : at.c_str(), false);
    }
    if (pathRow("Output folder (the folder 3ds inside goes to the root of the SD card)", &gChoice.out, "##out")) {
        SDL_ShowOpenFolderDialog(dialogDone, (void*) 3, window, gChoice.out.c_str(), false);
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("Textures:");
    ImGui::SameLine();
    ImGui::RadioButton("Pack and list (textures.pak + textures.pre, about 350 MB - smoother on the console)",
                       &gChoice.textures, 2);
    ImGui::Indent(ImGui::CalcTextSize("Textures: ").x);
    ImGui::RadioButton("List only (textures.pre, under a minute - the pack is in the folder already; after an update)",
                       &gChoice.textures, 1);
    ImGui::RadioButton("None", &gChoice.textures, 0);
    ImGui::Unindent(ImGui::CalcTextSize("Textures: ").x);
    ImGui::Checkbox("Movies (about 220 MB; needs ffmpeg with libx264)", &gChoice.movies);
    if (gChoice.movies && pathRow("ffmpeg", &gChoice.ffmpeg, "##ffmpeg")) {
        SDL_ShowOpenFileDialog(dialogDone, (void*) 4, window, kPrograms, (int) SDL_arraysize(kPrograms), nullptr, false);
    }
    if (gChoice.movies) {
        ffmpegStatus();
    }
    ImGui::Spacing();
    ImGui::Text("Saves to import (optional, .gci / .gcs from Dolphin or a copy tool): %d", (int) gChoice.saves.size());
    ImGui::SameLine();
    if (ImGui::Button("Add...")) {
        SDL_ShowOpenFileDialog(dialogDone, (void*) 5, window, kSaves, 2, nullptr, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        gChoice.saves.clear();
    }
    for (const std::string& s : gChoice.saves) {
        ImGui::BulletText("%s", fs::u8path(s).filename().u8string().c_str());
    }
    ImGui::Spacing();
    const bool discs = !gChoice.disc1.empty() || !gChoice.disc2.empty();
    const bool ready = !gChoice.out.empty() && (discs || gChoice.textures || gChoice.movies || !gChoice.saves.empty()) &&
                       (!gChoice.movies || ffmpegReady());  // (a missing ffmpeg found before the long unpacking)
    ImGui::BeginDisabled(!ready);
    const char* go = discs ? "Prepare" : "Prepare (from the data already in the folder)";
    if (ImGui::Button(go, ImVec2(std::max(ImGui::GetFontSize() * 10, ImGui::CalcTextSize(go).x +
                                                                          ImGui::GetStyle().FramePadding.x * 2),
                                 0))) {
        {
            std::lock_guard<std::mutex> l(gLock);
            gLines.clear();
            gProgress.clear();
            gFailure.clear();
        }
        gResult = -1;
        gBusy = true;
        std::thread(work, gChoice).detach();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        std::lock_guard<std::mutex> l(gLock);
        if (busy) {
            ImGui::TextUnformatted(gProgress.empty() ? "working..." : gProgress.c_str());
        } else if (gResult == 0) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "Done.");
        } else if (gResult == 1) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "Failed: %s", gFailure.empty() ? "see the log" : gFailure.c_str());
        }
    }
    if (!busy && gResult == 0) {
        ImGui::TextWrapped("Copy the folder \"3ds\" from %s to the root of your SD card (merge it with the 3ds folder "
                           "already there), with a card reader or over FTP.", gChoice.out.c_str());
        if (ImGui::Button("Open the output folder")) {
            SDL_OpenURL(("file://" + gChoice.out).c_str());
        }
    }
    ImGui::Separator();
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> l(gLock);
        for (const std::string& line : gLines) {
            ImGui::TextUnformatted(line.c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "PuebloPrep: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
#ifndef PUEBLOPREP_VERSION
#define PUEBLOPREP_VERSION "dev"
#endif
    SDL_Window* window = SDL_CreateWindow("PuebloPrep " PUEBLOPREP_VERSION, (int) (900 * scale), (int) (640 * scale),
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        fprintf(stderr, "PuebloPrep: no window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale * 1.25f;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    puebloprep_set_window(windowSay);
    if (const char* base = SDL_GetBasePath()) {  // puebloprep.log next to the program (from Finder the current folder is /)
        std::error_code ec;
        fs::path dir = fs::u8path(base);
        const std::string b = dir.u8string();
        const size_t app = b.find(".app/Contents/");
        fs::current_path(app != std::string::npos ? fs::u8path(b.substr(0, app + 4)).parent_path() : dir, ec);
    }
    gChoice.out = defaultOut();
    gChoice.ffmpeg = defaultFfmpeg();

    bool done = false;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            const bool close = e.type == SDL_EVENT_QUIT ||
                               (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window));
            if (close && gBusy) {  // (the work is not cut off half way: a folder half written)
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "PuebloPrep",
                                         "Still working - please wait until it is done.", window);
            } else if (close) {
                done = true;
            }
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(20);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        frame(window);
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, 0.1f, 0.1f, 0.11f, 1);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
