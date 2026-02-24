// Zeal ReaperSync REAPER extension - TCP server + song builder + marker sync

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_CountTracks
#define REAPERAPI_WANT_EnumProjects
#define REAPERAPI_WANT_SetProjExtState
#define REAPERAPI_WANT_GetProjExtState
#define REAPERAPI_WANT_NamedCommandLookup
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_Main_openProject
#define REAPERAPI_WANT_Main_SaveProject
#define REAPERAPI_WANT_Main_SaveProjectEx
#define REAPERAPI_WANT_OnPlayButton
#define REAPERAPI_WANT_OnStopButton
#define REAPERAPI_WANT_OnPauseButton
#define REAPERAPI_WANT_SetEditCurPos2
#define REAPERAPI_WANT_GetCursorPosition
#define REAPERAPI_WANT_GetSet_LoopTimeRange2
#define REAPERAPI_WANT_TimeMap_curFrameRate
#define REAPERAPI_WANT_AddProjectMarker2
#define REAPERAPI_WANT_SetProjectMarkerByIndex2
#define REAPERAPI_WANT_CountProjectMarkers
#define REAPERAPI_WANT_EnumProjectMarkers3
#define REAPERAPI_WANT_GetSetProjectInfo_String
#define REAPERAPI_WANT_GetSetProjectInfo
#define REAPERAPI_WANT_MarkProjectDirty
#define REAPERAPI_WANT_projectconfig_var_getoffs
#define REAPERAPI_WANT_projectconfig_var_addr
#define REAPERAPI_WANT_parse_timestr_len
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_GetProjectPath
#define REAPERAPI_WANT_GetSetMediaTrackInfo_String
#define REAPERAPI_WANT_InsertTrackAtIndex
#define REAPERAPI_WANT_GetTrack
#define REAPERAPI_WANT_AddMediaItemToTrack
#define REAPERAPI_WANT_AddTakeToMediaItem
#define REAPERAPI_WANT_SetMediaItemLength
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_SetMediaItemTake_Source
#define REAPERAPI_WANT_PCM_Source_CreateFromFile
#define REAPERAPI_WANT_GetMediaSourceLength
#define REAPERAPI_WANT_UpdateTimeline
#define REAPERAPI_WANT_SelectProjectInstance
#define REAPERAPI_WANT_SetTempoTimeSigMarker
#define REAPERAPI_WANT_GetTempoTimeSigMarker
#define REAPERAPI_WANT_CountTempoTimeSigMarkers
#define REAPERAPI_WANT_ColorToNative
#define REAPERAPI_WANT_CountTrackMediaItems
#define REAPERAPI_WANT_GetTrackMediaItem
#define REAPERAPI_WANT_DeleteTrackMediaItem
#define REAPERAPI_WANT_GetMediaItem
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaSourceFileName
#define REAPERAPI_WANT_GetSetMediaItemTakeInfo_String
#define REAPERAPI_WANT_GetMediaItemNumTakes
#define REAPERAPI_WANT_GetTake
#define REAPERAPI_WANT_TakeIsMIDI
#define REAPERAPI_WANT_MIDI_CountEvts
#define REAPERAPI_WANT_MIDI_GetNote
#define REAPERAPI_WANT_MIDI_GetProjTimeFromPPQPos
#define REAPERAPI_WANT_GetMediaTrackInfo_Value
#define REAPERAPI_WANT_ColorFromNative
#define REAPERAPI_WANT_GetTrackMIDINoteNameEx

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <future>
#include <vector>
#include <array>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <cctype>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>

static reaper_plugin_info_t* g_rec = nullptr;

//==============================================================
// TCP server state
//==============================================================

static std::atomic<bool> g_serverRunning{false};
static std::atomic<int>  g_listenSock{-1};
static std::atomic<int>  g_udpSock{-1};
static std::thread       g_serverThread;
static std::thread       g_udpThread;

struct QueuedCommand
{
    std::string raw;
    bool needs_response = false;
    std::shared_ptr<std::promise<std::string>> response;
};

static std::mutex                                 g_cmdMutex;
static std::deque<std::shared_ptr<QueuedCommand>> g_cmdQueue;

//==============================================================
// Helpers
//==============================================================

static void log_msg(const std::string& msg)
{
    if (ShowConsoleMsg)
        ShowConsoleMsg((msg + "\n").c_str());
}

static std::string escape_json_string(const std::string& in)
{
    std::string out;
    out.reserve(in.size() * 1.2);
    for (char c : in)
    {
        switch (c)
        {
            case '\\': out += "\\\\"; break;
            case '\"': out += "\\\""; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                out.push_back(c);
                break;
        }
    }
    return out;
}

static bool set_project_start_from_tc(ReaProject* proj, const std::string& tc)
{
    if (!proj || tc.empty())
        return false;
    const double tcSeconds = parse_timestr_len(tc.c_str(), 0.0, 5); // mode 5 = h:m:s:f
    int sz = 0;
    const int offs = projectconfig_var_getoffs("projtimeoffs", &sz);
    if (!offs || sz != (int)sizeof(double))
        return false;
    void* addr = projectconfig_var_addr(proj, offs);
    if (!addr)
        return false;
    *reinterpret_cast<double*>(addr) = tcSeconds;
    MarkProjectDirty(proj);
    UpdateTimeline();
    return true;
}

static double get_project_start_offset_seconds(ReaProject* proj)
{
    if (!proj)
        return 0.0;
    int sz = 0;
    const int offs = projectconfig_var_getoffs("projtimeoffs", &sz);
    if (!offs || sz != (int)sizeof(double))
        return GetSetProjectInfo(proj, "PROJECT_START_OFFSET", 0.0, false);
    void* addr = projectconfig_var_addr(proj, offs);
    if (!addr)
        return GetSetProjectInfo(proj, "PROJECT_START_OFFSET", 0.0, false);
    return *reinterpret_cast<double*>(addr);
}

static void upsert_first_tempo_marker(ReaProject* proj, double bpm, int ts_num, int ts_den)
{
    if (!proj)
        return;
    const int markerCount = CountTempoTimeSigMarkers ? CountTempoTimeSigMarkers(proj) : 0;
    const int targetIdx = markerCount > 0 ? 0 : -1;
    SetTempoTimeSigMarker(proj, targetIdx, 0.0, -1, -1, bpm, ts_num, ts_den, false);
}

static std::string trim(const std::string& s)
{
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
        ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(start, end - start);
}

static bool handle_goto_cue(const std::string& args)
{
    double startSec = 0.0;
    try
    {
        startSec = std::stod(args);
    }
    catch (...)
    {
        log_msg("GotoCue: invalid argument; expected seconds.");
        return false;
    }

    if (startSec < 0.0)
        startSec = 0.0;

    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
    {
        log_msg("GotoCue: no active project.");
        return false;
    }

    OnStopButton();
    SetEditCurPos2(proj, startSec, true, false);
    OnPlayButton();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "GotoCue -> %.3f sec", startSec);
    log_msg(buf);
    return true;
}

static bool handle_play_from_start(const std::string& args)
{
    double startSec = 0.0;
    if (!args.empty())
    {
        try { startSec = std::stod(args); } catch (...) { startSec = 0.0; }
        if (startSec < 0.0)
            startSec = 0.0;
    }

    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
    {
        log_msg("PlayFromStart: no active project.");
        return false;
    }

    OnStopButton();
    SetEditCurPos2(proj, startSec, true, false);
    OnPlayButton();

    char buf[128];
    std::snprintf(buf, sizeof(buf), "PlayFromStart -> %.3f sec", startSec);
    log_msg(buf);
    return true;
}

static bool handle_scrub(const std::string& args, bool forward)
{
    double delta = 0.5; // default scrub increment in seconds
    if (!args.empty())
    {
        try { delta = std::stod(args); } catch (...) { delta = 0.5; }
    }
    if (delta < 0.0)
        delta = 0.0;
    if (!forward)
        delta = -delta;

    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
    {
        log_msg("Scrub: no active project.");
        return false;
    }

    double cur = GetCursorPosition();
    double next = cur + delta;
    if (next < 0.0)
        next = 0.0;

    SetEditCurPos2(proj, next, true, true); // move cursor; keep playback state if already playing

    char buf[128];
    std::snprintf(buf, sizeof(buf), "Scrub %s -> %.3f sec (delta %.3f)",
                  forward ? "forward" : "backward", next, delta);
    log_msg(buf);
    return true;
}

static bool handle_zoom(bool zoomIn)
{
    // REAPER native actions for horizontal zoom.
    if (zoomIn)
        Main_OnCommand(1012, 0);
    else
        Main_OnCommand(1011, 0);
    log_msg(zoomIn ? "ZoomIn -> horizontal" : "ZoomOut -> horizontal");
    return true;
}

static std::string handle_time_selection_range()
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";

    double start=0.0, end=0.0;
    GetSet_LoopTimeRange2(proj, false, false, &start, &end, false);
    if (end < start)
        end = start;
    double len = end - start;
    if (len < 0.0)
        len = 0.0;

    char buf[256];
    std::snprintf(buf, sizeof(buf), "OK %.6f %.6f %.6f", start, end, len);
    return std::string(buf);
}

static std::string handle_time_selection_length()
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";
    double start=0.0, end=0.0;
    GetSet_LoopTimeRange2(proj, false, false, &start, &end, false);
    if (end < start)
        end = start;
    double len = end - start;
    if (len < 0.0)
        len = 0.0;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "OK %.6f", len);
    return std::string(buf);
}

static bool command_needs_response(const std::string& raw)
{
    std::string verb = raw;
    size_t sp = raw.find(' ');
    if (sp != std::string::npos)
        verb = raw.substr(0, sp);
    return (verb.rfind("RS_", 0) == 0) || (verb == "GetTimeSelection") || (verb == "GetTimeSelectionRange");
}

static void enqueue_command(const std::shared_ptr<QueuedCommand>& cmd)
{
    std::lock_guard<std::mutex> lock(g_cmdMutex);
    g_cmdQueue.push_back(cmd);
}

static std::string safe_slug(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    bool lastUnderscore = false;
    for (char c : text)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        {
            out.push_back(c);
            lastUnderscore = false;
        }
        else if (c == ' ' || c == '-' || c == '_')
        {
            if (!lastUnderscore)
                out.push_back('_');
            lastUnderscore = true;
        }
        else
        {
            if (!lastUnderscore)
                out.push_back('_');
            lastUnderscore = true;
        }
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    if (out.empty())
        out = "Untitled";
    return out;
}

static std::string trim_quotes(const std::string& s)
{
    size_t start = 0;
    size_t end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '"' || s[start] == '\''))
        ++start;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '"' || s[end - 1] == '\''))
        --end;
    return s.substr(start, end - start);
}

static std::string to_lower(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

static MediaTrack* find_or_create_track(ReaProject* proj, const std::vector<std::string>& names)
{
    int trackCount = CountTracks(proj);
    for (int i = 0; i < trackCount; ++i)
    {
        MediaTrack* tr = GetTrack(proj, i);
        char nameBuf[256] = {0};
        GetSetMediaTrackInfo_String(tr, "P_NAME", nameBuf, false);
        std::string nm = to_lower(std::string(nameBuf));
        for (const auto& n : names)
        {
            if (nm == to_lower(n))
                return tr;
        }
    }

    // create if missing
    int newIndex = trackCount;
    InsertTrackAtIndex(newIndex, true);
    MediaTrack* tr = GetTrack(proj, newIndex);
    if (tr && !names.empty())
        GetSetMediaTrackInfo_String(tr, "P_NAME", (char*)names.front().c_str(), true);
    return tr;
}

static void replace_track_items(ReaProject* proj, MediaTrack* tr, const std::string& filePath)
{
    if (!proj || !tr || filePath.empty())
        return;
    int itemCount = CountTrackMediaItems(tr);
    for (int i = itemCount - 1; i >= 0; --i)
    {
        MediaItem* it = GetTrackMediaItem(tr, i);
        if (it)
            DeleteTrackMediaItem(tr, it);
    }

    MediaItem* it = AddMediaItemToTrack(tr);
    if (!it) return;
    MediaItem_Take* take = AddTakeToMediaItem(it);
    if (!take) return;
    PCM_source* src = PCM_Source_CreateFromFile(filePath.c_str());
    if (src)
        SetMediaItemTake_Source(take, src);
    double len = src ? GetMediaSourceLength(src, nullptr) : 0.0;
    if (len <= 0.0) len = 1.0;
    SetMediaItemLength(it, len, false);
    SetMediaItemInfo_Value(it, "D_POSITION", 0.0);
}

static double parse_timecode(const std::string& tc, int frameRate)
{
    int hh = 0, mm = 0, ss = 0, ff = 0;
    std::sscanf(tc.c_str(), "%d:%d:%d:%d", &hh, &mm, &ss, &ff);
    double sec = hh * 3600.0 + mm * 60.0 + ss;
    if (frameRate > 0)
        sec += static_cast<double>(ff) / static_cast<double>(frameRate);
    return sec;
}

static uint32_t hex_to_native(const std::string& hex)
{
    int r = 255, g = 255, b = 255;
    if (hex.size() >= 7 && hex[0] == '#')
    {
        unsigned int rv=255, gv=255, bv=255;
        if (std::sscanf(hex.c_str()+1, "%02x%02x%02x", &rv, &gv, &bv) == 3)
        {
            r = (int)rv;
            g = (int)gv;
            b = (int)bv;
        }
    }
    return ColorToNative(r, g, b) | 0x1000000;
}

// Very small JSON helpers (assumes flat key/value, double-quoted keys)
static std::string json_extract_string(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos);
    if (pos == std::string::npos) return {};
    pos = json.find_first_not_of(" \t\"", pos + 1);
    if (pos == std::string::npos) return {};
    bool quoted = json[pos] == '"';
    if (quoted) pos += 0;
    size_t start = quoted ? pos + 1 : pos;
    size_t end = quoted ? json.find('"', start) : json.find_first_of(",}", start);
    if (end == std::string::npos) end = json.size();
    return json.substr(start, end - start);
}

static double json_extract_number(const std::string& json, const std::string& key, double def = 0.0)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return def;
    pos = json.find_first_of("0123456789-.", pos + 1);
    if (pos == std::string::npos) return def;
    return std::atof(json.c_str() + pos);
}

static std::string json_extract_object(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find('{', pos);
    if (pos == std::string::npos) return {};
    int depth = 0;
    size_t end = pos;
    for (; end < json.size(); ++end)
    {
        if (json[end] == '{') depth++;
        else if (json[end] == '}')
        {
            depth--;
            if (depth == 0)
            {
                return json.substr(pos, end - pos + 1);
            }
        }
    }
    return {};
}

static std::vector<std::string> json_extract_array_strings(const std::string& json, const std::string& key)
{
    std::vector<std::string> out;
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos);
    if (pos == std::string::npos) return out;
    size_t end = json.find(']', pos);
    if (end == std::string::npos) return out;
    std::string body = json.substr(pos + 1, end - pos - 1);
    size_t start = 0;
    while (start < body.size())
    {
        size_t q1 = body.find('"', start);
        if (q1 == std::string::npos) break;
        size_t q2 = body.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        out.push_back(body.substr(q1 + 1, q2 - q1 - 1));
        start = q2 + 1;
    }
    return out;
}

struct ImportEvent
{
    std::string evId;
    double timeSec{0.0};
    std::string label;
    std::string color_hex;
    int trackgroup{0};
    int trackno{0};
};

static std::vector<ImportEvent> parse_events(const std::string& json)
{
    std::vector<ImportEvent> out;
    size_t pos = json.find('[');
    while (pos != std::string::npos)
    {
        size_t brace = json.find('{', pos);
        if (brace == std::string::npos) break;
        size_t end = json.find('}', brace);
        if (end == std::string::npos) break;
        std::string chunk = json.substr(brace, end - brace + 1);
        ImportEvent ev;
        ev.evId = json_extract_string(chunk, "evId");
        ev.label = json_extract_string(chunk, "label");
        ev.color_hex = json_extract_string(chunk, "color_hex");
        ev.timeSec = json_extract_number(chunk, "timeSec", 0.0);
        ev.trackgroup = (int)json_extract_number(chunk, "trackgroup", 0.0);
        ev.trackno = (int)json_extract_number(chunk, "trackno", 0.0);
        out.push_back(ev);
        pos = end + 1;
    }
    return out;
}

static ReaProject* open_or_select_project(const std::string& projectPath)
{
    std::error_code ec;
    auto target = std::filesystem::weakly_canonical(projectPath, ec);
    if (ec)
        target = std::filesystem::path(projectPath);

    char buf[4096] = {0};
    for (int i = 0;; ++i)
    {
        ReaProject* p = EnumProjects(i, buf, sizeof(buf));
        if (!p)
            break;
        if (buf[0] == '\0')
            continue;
        auto existing = std::filesystem::weakly_canonical(buf, ec);
        if (!ec && existing == target)
        {
            SelectProjectInstance(p);
            return p;
        }
    }

    Main_OnCommand(40859, 0); // new project tab (ignore default template)
    Main_openProject((char*)projectPath.c_str());
    return EnumProjects(-1, nullptr, 0);
}

static ReaProject* find_open_project_by_guid(const std::string& projectGuid)
{
    if (projectGuid.empty())
        return nullptr;
    char pathBuf[4096] = {0};
    char guidBuf[256] = {0};
    for (int i = 0;; ++i)
    {
        ReaProject* p = EnumProjects(i, pathBuf, sizeof(pathBuf));
        if (!p)
            break;
        guidBuf[0] = 0;
        GetProjExtState(p, "ZealReaperSync", "projectGuid", guidBuf, sizeof(guidBuf));
        std::string existing = trim_quotes(guidBuf);
        if (!existing.empty() && existing == projectGuid)
        {
            SelectProjectInstance(p);
            return p;
        }
    }
    return nullptr;
}

static ReaProject* find_open_project_by_path(const std::string& projectPath)
{
    if (projectPath.empty())
        return nullptr;
    std::error_code ec;
    auto target = std::filesystem::weakly_canonical(projectPath, ec);
    if (ec)
        target = std::filesystem::path(projectPath);

    char pathBuf[4096] = {0};
    for (int i = 0;; ++i)
    {
        ReaProject* p = EnumProjects(i, pathBuf, sizeof(pathBuf));
        if (!p)
            break;
        if (pathBuf[0] == '\0')
            continue;
        auto existing = std::filesystem::weakly_canonical(pathBuf, ec);
        if (!ec && existing == target)
            return p;
    }
    return nullptr;
}

static ReaProject* find_open_project_by_path_select(const std::string& projectPath)
{
    ReaProject* p = find_open_project_by_path(projectPath);
    if (p)
        SelectProjectInstance(p);
    return p;
}

static ReaProject* find_open_project_by_guid_no_select(const std::string& projectGuid)
{
    if (projectGuid.empty())
        return nullptr;
    char pathBuf[4096] = {0};
    char guidBuf[256] = {0};
    for (int i = 0;; ++i)
    {
        ReaProject* p = EnumProjects(i, pathBuf, sizeof(pathBuf));
        if (!p)
            break;
        guidBuf[0] = 0;
        GetProjExtState(p, "ZealReaperSync", "projectGuid", guidBuf, sizeof(guidBuf));
        std::string existing = trim_quotes(guidBuf);
        if (!existing.empty() && existing == projectGuid)
            return p;
    }
    return nullptr;
}

static std::string handle_open_project(const std::string& json)
{
    std::string projectPath = trim_quotes(json_extract_string(json, "projectPath"));
    std::string projectGuid = trim_quotes(json_extract_string(json, "projectGuid"));

    if (!projectGuid.empty())
    {
        if (find_open_project_by_guid_no_select(projectGuid))
            return "OK ALREADY_OPEN GUID";
    }
    if (!projectPath.empty())
    {
        if (find_open_project_by_path(projectPath))
            return "OK ALREADY_OPEN PATH";
    }

    if (projectPath.empty())
        return "ERR MissingProjectPath";
    if (!std::filesystem::exists(projectPath))
        return "ERR ProjectNotFound";

    Main_OnCommand(40859, 0); // new project tab
    Main_openProject((char*)projectPath.c_str());
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR OpenFailed";

    if (!projectGuid.empty())
        SetProjExtState(proj, "ZealReaperSync", "projectGuid", projectGuid.c_str());
    return "OK OPENED";
}

static std::string handle_apply_update(const std::string& json)
{
    std::string projectPath = trim_quotes(json_extract_string(json, "projectPath"));
    std::string projectGuid = trim_quotes(json_extract_string(json, "projectGuid"));

    double bpm  = json_extract_number(json, "bpm", 120.0);
    int ts_num  = (int)json_extract_number(json, "timesig_num", 4);
    int ts_den  = (int)json_extract_number(json, "timesig_den", 4);
    int frameRate = (int)json_extract_number(json, "frameRate", 25);
    std::string startTc = json_extract_string(json, "startTc");
    std::string audioObj = json_extract_object(json, "audio");
    std::string foh = trim_quotes(json_extract_string(audioObj, "foh"));
    std::string ltc = trim_quotes(json_extract_string(audioObj, "ltc"));
    int songId = (int)json_extract_number(json, "songId", 0);
    std::string stateJson = json_extract_string(json, "state");

    ReaProject* proj = nullptr;
    if (!projectGuid.empty())
        proj = find_open_project_by_guid(projectGuid);
    if (!proj)
    {
        if (projectPath.empty())
            return "ERR Missing projectPathOrGuid";
        if (!std::filesystem::exists(projectPath))
            return "ERR ProjectNotFound";
        proj = open_or_select_project(projectPath);
    }
    if (!proj)
        return "ERR NoProject";

    set_project_start_from_tc(proj, startTc);
    upsert_first_tempo_marker(proj, bpm, ts_num, ts_den);

    if (songId > 0)
        SetProjExtState(proj, "ZealReaperSync", "songId", std::to_string(songId).c_str());
    if (!projectGuid.empty())
        SetProjExtState(proj, "ZealReaperSync", "projectGuid", projectGuid.c_str());
    if (!stateJson.empty())
        SetProjExtState(proj, "ZealReaperSync", "state", stateJson.c_str());

    if (!ltc.empty())
    {
        MediaTrack* tcTrack = find_or_create_track(proj, {"TC", "LTC"});
        replace_track_items(proj, tcTrack, ltc);
    }
    if (!foh.empty())
    {
        MediaTrack* fohTrack = find_or_create_track(proj, {"FOH", "TRACK"});
        replace_track_items(proj, fohTrack, foh);
    }

    UpdateTimeline();
    Main_SaveProject(proj, false);
    log_msg(std::string("RS_APPLY_UPDATE OK -> ") + projectPath);
    return "OK";
}

static std::string handle_reorder_open_projects(const std::string& json)
{
    std::vector<std::string> projectPaths = json_extract_array_strings(json, "projectPaths");
    if (projectPaths.empty())
        return "ERR MissingProjectPaths";

    std::vector<std::string> existing;
    existing.reserve(projectPaths.size());
    for (const auto& raw : projectPaths)
    {
        std::string p = trim_quotes(raw);
        if (p.empty())
            continue;
        if (!std::filesystem::exists(p))
            continue;
        existing.push_back(p);
    }
    if (existing.empty())
        return "ERR NoExistingProjectPaths";

    // Reorder by closing any currently-open target tabs, then reopening in desired order.
    // Action 40860 = File: Close current project tab.
    for (const auto& path : existing)
    {
        ReaProject* open = find_open_project_by_path_select(path);
        if (open)
            Main_OnCommand(40860, 0);
    }

    int reopened = 0;
    for (const auto& path : existing)
    {
        Main_OnCommand(40859, 0); // new project tab
        Main_openProject((char*)path.c_str());
        ReaProject* proj = EnumProjects(-1, nullptr, 0);
        if (proj)
            reopened++;
    }

    return std::string("OK REORDERED ") + std::to_string(reopened);
}

static std::string handle_get_project_snapshot(const std::string& json)
{
    std::string projectPath = trim_quotes(json_extract_string(json, "projectPath"));
    ReaProject* proj = nullptr;
    if (!projectPath.empty())
    {
        proj = open_or_select_project(projectPath);
    }
    else
    {
        proj = EnumProjects(-1, nullptr, 0);
    }
    if (!proj)
        return "ERR NoProject";

    char pathBuf[4096] = {0};
    EnumProjects(-1, pathBuf, sizeof(pathBuf));

    double start_offset = get_project_start_offset_seconds(proj);
    double bpm = 120.0;
    int ts_num = 4, ts_den = 4;
    double pos = 0.0, beatPos = 0.0;
    int measurePos = 0;
    GetTempoTimeSigMarker(proj, 0, &pos, &measurePos, &beatPos, &bpm, &ts_num, &ts_den, nullptr);

    char songBuf[128] = {0};
    GetProjExtState(proj, "ZealReaperSync", "songId", songBuf, sizeof(songBuf));
    char guidBuf[256] = {0};
    GetProjExtState(proj, "ZealReaperSync", "projectGuid", guidBuf, sizeof(guidBuf));
    char stateBuf[4096] = {0};
    GetProjExtState(proj, "ZealReaperSync", "state", stateBuf, sizeof(stateBuf));

    std::string stateField = "null";
    if (stateBuf[0])
        stateField = std::string("\"") + escape_json_string(stateBuf) + "\"";

    auto find_track_by_any_name = [&](const std::vector<std::string>& names) -> MediaTrack*
    {
        int trackCount = CountTracks(proj);
        for (int i = 0; i < trackCount; ++i)
        {
            MediaTrack* tr = GetTrack(proj, i);
            if (!tr)
                continue;
            char nameBuf[256] = {0};
            GetSetMediaTrackInfo_String(tr, "P_NAME", nameBuf, false);
            std::string current = to_lower(std::string(nameBuf));
            for (const auto& name : names)
            {
                if (current == to_lower(name))
                    return tr;
            }
        }
        return nullptr;
    };

    auto track_source_path = [&](MediaTrack* tr) -> std::string
    {
        if (!tr)
            return {};
        int itemCount = CountTrackMediaItems(tr);
        if (itemCount <= 0)
            return {};
        MediaItem* item = GetTrackMediaItem(tr, 0);
        if (!item)
            return {};
        MediaItem_Take* take = GetActiveTake(item);
        if (!take)
            return {};
        PCM_source* src = GetMediaItemTake_Source(take);
        if (!src)
            return {};
        char srcBuf[4096] = {0};
        GetMediaSourceFileName(src, srcBuf, sizeof(srcBuf));
        return std::string(srcBuf);
    };

    std::string tcSource = track_source_path(find_track_by_any_name({"TC", "LTC"}));
    std::string trackSource = track_source_path(find_track_by_any_name({"TRACK", "FOH"}));

    char buf[16384];
    std::snprintf(
        buf,
        sizeof(buf),
        "{\"projectPath\":\"%s\",\"songId\":%d,\"projectGuid\":\"%s\",\"start_offset\":%.6f,\"bpm\":%.3f,\"ts_num\":%d,\"ts_den\":%d,\"tc_source\":\"%s\",\"track_source\":\"%s\",\"state\":%s}",
        pathBuf,
        std::atoi(songBuf),
        guidBuf,
        start_offset,
        bpm,
        ts_num,
        ts_den,
        escape_json_string(tcSource).c_str(),
        escape_json_string(trackSource).c_str(),
        stateField.c_str()
    );
    return std::string("OK ") + buf;
}

//==============================================================
// MIDI note export (for MA3 sync)
//==============================================================

static std::string default_note_name(int midiNote)
{
    static const char* kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int n = midiNote;
    if (n < 0) n = 0;
    if (n > 127) n = 127;
    int octave = (n / 12) - 2; // REAPER/MIDI convention
    int pitch = n % 12;
    return std::string(kNames[pitch]) + std::to_string(octave);
}

static std::string json_note_name_for_track(MediaTrack* tr, int midiNote, int channelZeroBased)
{
    const int chan = channelZeroBased < 0 ? 0 : channelZeroBased;
    if (GetTrackMIDINoteNameEx)
    {
        if (const char* named = GetTrackMIDINoteNameEx(nullptr, tr, midiNote, chan))
        {
            std::string v = trim(named);
            if (!v.empty())
                return v;
        }
    }
    return default_note_name(midiNote);
}

static std::string color_native_to_hex(int nativeColor)
{
    int c = nativeColor;
    if (c == 0)
        return "#FFFFFF";
    c &= 0x00FFFFFF; // strip custom-color flag bit
    int r = 255, g = 255, b = 255;
    if (ColorFromNative)
        ColorFromNative(c, &r, &g, &b);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r & 0xFF, g & 0xFF, b & 0xFF);
    return std::string(buf);
}

static std::string handle_export_midi_note_events(const std::string& json)
{
    std::string projectPath = trim_quotes(json_extract_string(json, "projectPath"));
    std::string projectGuid = trim_quotes(json_extract_string(json, "projectGuid"));

    ReaProject* proj = nullptr;
    if (!projectGuid.empty())
        proj = find_open_project_by_guid(projectGuid);
    if (!proj && !projectPath.empty())
        proj = open_or_select_project(projectPath);
    if (!proj)
        proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";

    char projPathBuf[4096] = {0};
    EnumProjects(-1, projPathBuf, sizeof(projPathBuf));
    char songIdBuf[128] = {0};
    GetProjExtState(proj, "ZealReaperSync", "songId", songIdBuf, sizeof(songIdBuf));
    int songId = std::atoi(songIdBuf);
    char guidBuf[256] = {0};
    GetProjExtState(proj, "ZealReaperSync", "projectGuid", guidBuf, sizeof(guidBuf));

    char projNameBuf[512] = {0};
    GetSetProjectInfo_String(proj, "PROJECT_NAME", projNameBuf, false);
    std::string projectName = trim(std::string(projNameBuf));
    if (projectName.empty())
    {
        std::filesystem::path p(projPathBuf);
        projectName = p.filename().string();
    }

    std::string outJson = "{";
    outJson += "\"song_id\":" + std::to_string(songId) + ",";
    outJson += "\"project_guid\":\"" + escape_json_string(std::string(guidBuf)) + "\",";
    outJson += "\"project_name\":\"" + escape_json_string(projectName) + "\",";
    outJson += "\"project_path\":\"" + escape_json_string(std::string(projPathBuf)) + "\",";
    {
        char offsBuf[64];
        std::snprintf(offsBuf, sizeof(offsBuf), "%.6f", get_project_start_offset_seconds(proj));
        outJson += "\"project_start_offset_sec\":";
        outJson += offsBuf;
        outJson += ",";
    }
    outJson += "\"tracks\":[";

    bool firstTrackOut = true;
    const int trackCount = CountTracks(proj);
    for (int ti = 0; ti < trackCount; ++ti)
    {
        MediaTrack* tr = GetTrack(proj, ti);
        if (!tr)
            continue;

        char trackNameBuf[512] = {0};
        GetSetMediaTrackInfo_String(tr, "P_NAME", trackNameBuf, false);
        std::string trackName = trim(std::string(trackNameBuf));
        if (trackName.empty())
            trackName = "Track_" + std::to_string(ti + 1);

        const int nativeColor = (int)GetMediaTrackInfo_Value(tr, "I_CUSTOMCOLOR");
        const std::string trackColorHex = color_native_to_hex(nativeColor);

        // Accumulate hit times by MIDI note per track.
        std::array<std::vector<double>, 128> noteHits;
        std::array<int, 128> noteFirstChannel;
        noteFirstChannel.fill(0);

        const int itemCount = CountTrackMediaItems(tr);
        for (int ii = 0; ii < itemCount; ++ii)
        {
            MediaItem* item = GetTrackMediaItem(tr, ii);
            if (!item)
                continue;
            const int takeCount = GetMediaItemNumTakes(item);
            for (int tk = 0; tk < takeCount; ++tk)
            {
                MediaItem_Take* take = GetTake(item, tk);
                if (!take || !TakeIsMIDI(take))
                    continue;
                int noteCount = 0, ccCount = 0, textCount = 0;
                if (!MIDI_CountEvts(take, &noteCount, &ccCount, &textCount) || noteCount <= 0)
                    continue;

                for (int ni = 0; ni < noteCount; ++ni)
                {
                    bool selected = false, muted = false;
                    double startPpq = 0.0, endPpq = 0.0;
                    int chan = 0, pitch = 0, vel = 0;
                    if (!MIDI_GetNote(take, ni, &selected, &muted, &startPpq, &endPpq, &chan, &pitch, &vel))
                        continue;
                    if (pitch < 0 || pitch > 127)
                        continue;
                    const double timeSec = MIDI_GetProjTimeFromPPQPos(take, startPpq);
                    noteHits[pitch].push_back(timeSec);
                    if (noteHits[pitch].size() == 1)
                        noteFirstChannel[pitch] = chan;
                }
            }
        }

        bool anyNotes = false;
        for (int n = 0; n < 128; ++n)
        {
            if (!noteHits[n].empty())
            {
                anyNotes = true;
                break;
            }
        }
        if (!anyNotes)
            continue;

        if (!firstTrackOut)
            outJson += ",";
        firstTrackOut = false;

        outJson += "{";
        outJson += "\"track_name\":\"" + escape_json_string(trackName) + "\",";
        outJson += "\"track_color\":\"" + escape_json_string(trackColorHex) + "\",";
        outJson += "\"notes\":[";

        bool firstNoteOut = true;
        for (int n = 0; n < 128; ++n)
        {
            auto& hits = noteHits[n];
            if (hits.empty())
                continue;
            std::sort(hits.begin(), hits.end());
            const std::string noteName = json_note_name_for_track(tr, n, noteFirstChannel[n]);

            if (!firstNoteOut)
                outJson += ",";
            firstNoteOut = false;

            outJson += "{";
            outJson += "\"note\":" + std::to_string(n) + ",";
            outJson += "\"note_name\":\"" + escape_json_string(noteName) + "\",";
            outJson += "\"channel\":" + std::to_string(noteFirstChannel[n] + 1) + ",";
            outJson += "\"hits\":[";
            for (size_t hi = 0; hi < hits.size(); ++hi)
            {
                char tsBuf[64];
                std::snprintf(tsBuf, sizeof(tsBuf), "%.6f", hits[hi]);
                if (hi > 0)
                    outJson += ",";
                outJson += tsBuf;
            }
            outJson += "]";
            outJson += "}";
        }

        outJson += "]";
        outJson += "}";
    }

    outJson += "],";

    outJson += "\"markers\":[";
    bool firstMarkerOut = true;
    int numMarkers = 0, numRegions = 0;
    CountProjectMarkers(proj, &numMarkers, &numRegions);
    const int total = numMarkers + numRegions;
    for (int i = 0; i < total; ++i)
    {
        bool isrgn = false;
        double pos = 0.0, rgnend = 0.0;
        const char* name = nullptr;
        int idnum = 0, colorRaw = 0;
        if (!EnumProjectMarkers3(proj, i, &isrgn, &pos, &rgnend, &name, &idnum, &colorRaw))
            continue;
        if (isrgn)
            continue;
        if (!firstMarkerOut)
            outJson += ",";
        firstMarkerOut = false;
        char posBuf[64];
        std::snprintf(posBuf, sizeof(posBuf), "%.6f", pos);
        outJson += "{";
        outJson += "\"name\":\"" + escape_json_string(name ? std::string(name) : std::string()) + "\",";
        outJson += "\"time_sec\":" + std::string(posBuf) + ",";
        outJson += "\"color_hex\":\"" + escape_json_string(color_native_to_hex(colorRaw)) + "\"";
        outJson += "}";
    }
    outJson += "]";
    outJson += ",";

    outJson += "\"tempo_markers\":[";
    bool firstTempoOut = true;
    const int tempoCount = CountTempoTimeSigMarkers ? CountTempoTimeSigMarkers(proj) : 0;
    for (int i = 0; i < tempoCount; ++i)
    {
        double tpos = 0.0, beatPos = 0.0, bpm = 120.0;
        int measurePos = 0, tsNum = 4, tsDen = 4;
        const bool ok = GetTempoTimeSigMarker
            ? GetTempoTimeSigMarker(proj, i, &tpos, &measurePos, &beatPos, &bpm, &tsNum, &tsDen, nullptr)
            : false;
        if (!ok)
            continue;
        if (!firstTempoOut)
            outJson += ",";
        firstTempoOut = false;
        char posBuf[64];
        char bpmBuf[64];
        char bpm2Buf[64];
        std::snprintf(posBuf, sizeof(posBuf), "%.6f", tpos);
        std::snprintf(bpmBuf, sizeof(bpmBuf), "%.6f", bpm);
        std::snprintf(bpm2Buf, sizeof(bpm2Buf), "%.2f", bpm);
        outJson += "{";
        outJson += "\"time_sec\":" + std::string(posBuf) + ",";
        outJson += "\"bpm\":" + std::string(bpmBuf) + ",";
        outJson += "\"bpm_str\":\"" + escape_json_string(std::string(bpm2Buf)) + "\",";
        outJson += "\"timesig_num\":" + std::to_string(tsNum) + ",";
        outJson += "\"timesig_den\":" + std::to_string(tsDen);
        outJson += "}";
    }
    outJson += "]";
    outJson += "}";
    return std::string("OK ") + outJson;
}

//==============================================================
// Track/media helpers
//==============================================================

static MediaTrack* find_track_by_name(ReaProject* proj, const std::string& name)
{
    int cnt = CountTracks(proj);
    char buf[256];
    for (int i = 0; i < cnt; ++i)
    {
        MediaTrack* tr = GetTrack(proj, i);
        if (!tr) continue;
        buf[0] = 0;
        GetSetMediaTrackInfo_String(tr, "P_NAME", buf, false);
        if (name == buf)
            return tr;
    }
    return nullptr;
}

static MediaTrack* ensure_named_track_at(ReaProject* proj, int index, const std::string& name)
{
    int cnt = CountTracks(proj);
    if (index < cnt)
    {
        MediaTrack* tr = GetTrack(proj, index);
        if (tr)
        {
            char buf[256];
            buf[0] = 0;
            GetSetMediaTrackInfo_String(tr, "P_NAME", buf, false);
            std::string current = buf;
            if (current != name)
            {
                std::snprintf(buf, sizeof(buf), "%s", name.c_str());
                GetSetMediaTrackInfo_String(tr, "P_NAME", buf, true);
            }
            return tr;
        }
    }
    int insertIdx = (index < 0) ? cnt : index;
    InsertTrackAtIndex(insertIdx, true);
    MediaTrack* tr = GetTrack(proj, insertIdx);
    if (tr)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s", name.c_str());
        GetSetMediaTrackInfo_String(tr, "P_NAME", buf, true);
    }
    return tr;
}

static bool insert_media_on_track(MediaTrack* tr, const std::string& path, double startPos)
{
    if (!tr) return false;
    PCM_source* src = PCM_Source_CreateFromFile(path.c_str());
    if (!src) return false;
    MediaItem* item = AddMediaItemToTrack(tr);
    if (!item) return false;
    MediaItem_Take* take = AddTakeToMediaItem(item);
    if (!take) return false;
    SetMediaItemTake_Source(take, src);
    double len = GetMediaSourceLength(src, nullptr);
    SetMediaItemLength(item, len, false);
    SetMediaItemInfo_Value(item, "D_POSITION", startPos);
    return true;
}

//==============================================================
// Marker helpers and extstate links
//==============================================================

struct LinkRecord
{
    std::string marker_guid;
    std::string ma3_evId;
    int trackgroup{0};
    int trackno{0};
    std::string color_hex;
    std::string source{"MA3"};
    double last_seen{0.0};
};

static std::vector<LinkRecord> load_links(ReaProject* proj)
{
    std::vector<LinkRecord> records;
    char buf[8192] = {};
    int sz = GetProjExtState(proj, "ZealReaperSync", "links", buf, sizeof(buf));
    if (sz <= 0)
        return records;
    std::string body(buf, sz);
    size_t start = 0;
    while (start < body.size())
    {
        size_t end = body.find('\n', start);
        if (end == std::string::npos) end = body.size();
        std::string line = body.substr(start, end - start);
        if (!line.empty())
        {
            LinkRecord rec;
            int tg = 0, tn = 0;
            double lastSeen = 0.0;
            char guid[128] = {}, ev[128] = {}, color[64] = {}, source[32] = {};
            if (std::sscanf(line.c_str(), "%127[^\t]\t%127[^\t]\t%d\t%d\t%63[^\t]\t%lf\t%31[^\t]",
                            guid, ev, &tg, &tn, color, &lastSeen, source) >= 2)
            {
                rec.marker_guid = guid;
                rec.ma3_evId = ev;
                rec.trackgroup = tg;
                rec.trackno = tn;
                rec.color_hex = color;
                rec.last_seen = lastSeen;
                rec.source = source;
                records.push_back(rec);
            }
        }
        start = end + 1;
    }
    return records;
}

static void save_links(ReaProject* proj, const std::vector<LinkRecord>& records)
{
    std::string body;
    for (const auto& rec : records)
    {
        char line[512];
        std::snprintf(line, sizeof(line), "%s\t%s\t%d\t%d\t%s\t%.0f\t%s\n",
                      rec.marker_guid.c_str(),
                      rec.ma3_evId.c_str(),
                      rec.trackgroup,
                      rec.trackno,
                      rec.color_hex.c_str(),
                      rec.last_seen,
                      rec.source.c_str());
        body.append(line);
    }
    SetProjExtState(proj, "ZealReaperSync", "links", body.c_str());
}

static std::string links_to_json(const std::vector<LinkRecord>& records)
{
    std::string out = "{";
    for (size_t i = 0; i < records.size(); ++i)
    {
        const auto& r = records[i];
        out += "\"" + r.marker_guid + "\":{";
        out += "\"source\":\"" + r.source + "\",";
        out += "\"ma3_evId\":\"" + r.ma3_evId + "\",";
        out += "\"trackgroup\":" + std::to_string(r.trackgroup) + ",";
        out += "\"trackno\":" + std::to_string(r.trackno) + ",";
        out += "\"last_seen\":" + std::to_string((long long)r.last_seen) + ",";
        out += "\"color_hex\":\"" + r.color_hex + "\"";
        out += "}";
        if (i + 1 < records.size()) out += ",";
    }
    out += "}";
    return out;
}

static std::string marker_guid_for_index(ReaProject* proj, int enumIndex)
{
    if (!GetSetProjectInfo_String)
        return {};
    char param[32];
    std::snprintf(param, sizeof(param), "MARKER_GUID:%d", enumIndex);
    char guidBuf[128] = {};
    if (GetSetProjectInfo_String(proj, param, guidBuf, false))
        return std::string(guidBuf);
    return {};
}

static void inject_metadata_header(const std::filesystem::path& projPath,
                                   int songId,
                                   const std::string& songName)
{
    std::error_code ec;
    if (!std::filesystem::exists(projPath, ec))
        return;
    std::string markerId = "# CSLD_SONG_ID " + std::to_string(songId);
    std::string markerName = "# CSLD_SONG_NAME " + songName;
    std::ifstream in(projPath);
    if (!in)
        return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (content.find(markerId) != std::string::npos || content.find(markerName) != std::string::npos)
        return;
    std::ofstream out(projPath, std::ios::trunc);
    if (!out)
        return;
    out << markerId << "\n" << markerName << "\n\n" << content;
}

static int find_marker_index_by_guid(ReaProject* proj, const std::string& guid, int& out_idnumber, int& out_color)
{
    int numMarkers=0, numRegions=0;
    CountProjectMarkers(proj, &numMarkers, &numRegions);
    int total = numMarkers + numRegions;
    for (int i = 0; i < total; ++i)
    {
        bool isrgn=false;
        double pos=0.0, end=0.0;
        const char* name=nullptr;
        int idnum=0, color=0;
        if (!EnumProjectMarkers3(proj, i, &isrgn, &pos, &end, &name, &idnum, &color))
            continue;
        if (isrgn) continue;
        std::string g = marker_guid_for_index(proj, i);
        if (g == guid)
        {
            out_idnumber = idnum;
            out_color = color;
            return i;
        }
    }
    return -1;
}

static int find_marker_index_by_token(ReaProject* proj, const std::string& token, int& out_idnum)
{
    int numMarkers=0, numRegions=0;
    CountProjectMarkers(proj, &numMarkers, &numRegions);
    int total = numMarkers + numRegions;
    for (int i = total - 1; i >=0; --i)
    {
        bool isrgn=false;
        double pos=0.0, end=0.0;
        const char* name=nullptr;
        int idnum=0, color=0;
        if (!EnumProjectMarkers3(proj, i, &isrgn, &pos, &end, &name, &idnum, &color))
            continue;
        if (isrgn) continue;
        if (name && std::strstr(name, token.c_str()))
        {
            out_idnum = idnum;
            return i;
        }
    }
    return -1;
}

//==============================================================
// Build song
//==============================================================

static std::string handle_build_song(const std::string& json)
{
    std::string targetFolder = trim_quotes(json_extract_string(json, "targetFolder"));
    std::string songName     = json_extract_string(json, "songName");
    int songId               = (int)json_extract_number(json, "songId", 0.0);
    std::string templatePath = json_extract_string(json, "templatePath");
    double bpm               = json_extract_number(json, "bpm", 120.0);
    int ts_num               = (int)json_extract_number(json, "timesig_num", 4);
    int ts_den               = (int)json_extract_number(json, "timesig_den", 4);
    int frameRate            = (int)json_extract_number(json, "frameRate", 25);
    std::string startTc      = json_extract_string(json, "startTc");
    std::string projectGuid  = trim_quotes(json_extract_string(json, "projectGuid"));
    std::string audioObj     = json_extract_object(json, "audio");
    std::string foh          = trim_quotes(json_extract_string(audioObj, "foh"));
    std::string ltc          = trim_quotes(json_extract_string(audioObj, "ltc"));

    if (templatePath.empty() || targetFolder.empty())
        return "ERR Missing template or target folder";

    if (!std::filesystem::exists(templatePath))
    {
        std::filesystem::path alt = templatePath;
        if (alt.has_extension())
        {
            std::string ext = alt.extension().string();
            if (ext == ".rpp")
                alt.replace_extension(".RPP");
            else if (ext == ".RPP")
                alt.replace_extension(".rpp");
            else
                alt = alt.replace_extension(".rpp");
        }
        else
        {
            alt += ".rpp";
        }
        if (std::filesystem::exists(alt))
            templatePath = alt.string();
        else
            return "ERR Template not found";
    }

    std::filesystem::path tgtDir(targetFolder);
    std::filesystem::path mediaDir = tgtDir / "Media";
    std::error_code ec;
    std::filesystem::create_directories(mediaDir, ec);

    // Determine destination path and copy template there first
    std::filesystem::path destProject = tgtDir / (safe_slug(songName) + ".rpp");
    std::filesystem::copy_file(templatePath, destProject, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return "ERR FailedToCopyTemplate";

    // new tab and open the copied project
    Main_OnCommand(40859, 0);
    Main_openProject((char*)destProject.string().c_str());
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR CouldNotOpenProject";

    auto copy_if_exists = [&](const std::string& srcPath) -> std::string {
        std::string cleaned = trim_quotes(srcPath);
        if (cleaned.empty()) return {};
        std::filesystem::path src(cleaned);
        if (!std::filesystem::exists(src))
            return {};
        std::filesystem::path dest = mediaDir / src.filename();
        std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing, ec);
        return dest.string();
    };

    std::string fohDest = copy_if_exists(foh);
    std::string ltcDest = copy_if_exists(ltc);

    set_project_start_from_tc(proj, startTc);
    upsert_first_tempo_marker(proj, bpm, ts_num, ts_den);
    if (songId > 0)
        SetProjExtState(proj, "ZealReaperSync", "songId", std::to_string(songId).c_str());
    if (!projectGuid.empty())
        SetProjExtState(proj, "ZealReaperSync", "projectGuid", projectGuid.c_str());
    // Set project name (display) to songName
    GetSetProjectInfo_String(proj, "PROJECT_NAME", (char*)songName.c_str(), true);

    if (!ltcDest.empty())
    {
        MediaTrack* tcTrack = find_or_create_track(proj, {"TC", "LTC"});
        replace_track_items(proj, tcTrack, ltcDest);
    }
    if (!fohDest.empty())
    {
        MediaTrack* fohTrack = find_or_create_track(proj, {"FOH", "TRACK"});
        replace_track_items(proj, fohTrack, fohDest);
    }

    GetSetProjectInfo_String(proj, "PROJECT_NAME", (char*)songName.c_str(), true);
    GetSetProjectInfo_String(proj, "PROJECT_FILE", (char*)destProject.string().c_str(), true);
    Main_SaveProjectEx(proj, destProject.string().c_str(), true);
    if (!std::filesystem::exists(destProject))
        return "ERR SaveFailed";
    auto sz = std::filesystem::file_size(destProject, ec);
    if (ec || sz == 0)
        return "ERR SaveFailedEmpty";
    UpdateTimeline();
    log_msg(std::string("RS_BUILD_SONG OK -> ") + destProject.string());
    return "OK";
}

//==============================================================
// Marker exports
//==============================================================

static std::string handle_get_markers()
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";

    int numMarkers=0, numRegions=0;
    CountProjectMarkers(proj, &numMarkers, &numRegions);
    int total = numMarkers + numRegions;
    std::string json = "[";
    bool first = true;
    for (int i = 0; i < total; ++i)
    {
        bool isrgn=false;
        double pos=0.0, end=0.0;
        const char* name=nullptr;
        int idnum=0, color=0;
        if (!EnumProjectMarkers3(proj, i, &isrgn, &pos, &end, &name, &idnum, &color))
            continue;
        if (isrgn) continue;
        std::string guid = marker_guid_for_index(proj, i);
        if (!first) json += ",";
        first = false;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "{\"marker_index\":%d,\"guid\":\"%s\",\"pos_sec\":%.6f,\"name\":\"%s\",\"color_raw\":%d}",
                      idnum,
                      guid.c_str(),
                      pos,
                      name ? name : "",
                      color);
        json += buf;
    }
    json += "]";
    return "OK " + json;
}

static uint32_t color_code_to_native(const std::string& colorCode)
{
    int a = 0, b = 0, c = 0;
    if (std::sscanf(colorCode.c_str(), "%d,%d,%d", &a, &b, &c) == 3)
    {
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        if (c < 0) c = 0;
        if (c > 255) c = 255;
        return ColorToNative(a, b, c) | 0x1000000;
    }
    return ColorToNative(255, 255, 255) | 0x1000000;
}

static int find_marker_enum_index_by_idnum(ReaProject* proj, int markerIdNum)
{
    int numMarkers = 0;
    int numRegions = 0;
    CountProjectMarkers(proj, &numMarkers, &numRegions);
    const int total = numMarkers + numRegions;
    for (int i = 0; i < total; ++i)
    {
        bool isrgn = false;
        double pos = 0.0, end = 0.0;
        const char* name = nullptr;
        int idnum = 0, color = 0;
        if (!EnumProjectMarkers3(proj, i, &isrgn, &pos, &end, &name, &idnum, &color))
            continue;
        if (isrgn)
            continue;
        if (idnum == markerIdNum)
            return i;
    }
    return -1;
}

static std::string handle_get_playhead()
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";
    const double playheadSec = GetCursorPosition();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "OK {\"playhead_sec\":%.6f}", playheadSec < 0.0 ? 0.0 : playheadSec);
    return std::string(buf);
}

static std::string handle_create_note_marker(const std::string& json)
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";

    std::string name = json_extract_string(json, "name");
    if (name.empty())
        name = "USER: Note";
    double posSec = json_extract_number(json, "pos_sec", 0.0);
    if (posSec < 0.0)
        posSec = 0.0;
    std::string colorCode = trim(json_extract_string(json, "color_code"));
    if (colorCode.empty())
        colorCode = "0,0,1";
    const uint32_t colorNative = color_code_to_native(colorCode);

    const int markerId = AddProjectMarker2(proj, false, posSec, 0.0, name.c_str(), -1, colorNative);
    if (markerId < 0)
        return "ERR MarkerCreateFailed";

    const int enumIdx = find_marker_enum_index_by_idnum(proj, markerId);
    std::string guid;
    if (enumIdx >= 0)
        guid = marker_guid_for_index(proj, enumIdx);

    char out[1024];
    std::snprintf(
        out,
        sizeof(out),
        "OK {\"idnum\":%d,\"guid\":\"%s\",\"pos_sec\":%.6f,\"name\":\"%s\",\"color_code\":\"%s\"}",
        markerId,
        escape_json_string(guid).c_str(),
        posSec,
        escape_json_string(name).c_str(),
        escape_json_string(colorCode).c_str()
    );
    return std::string(out);
}

static std::string handle_goto_and_play(const std::string& json)
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";

    double targetSec = json_extract_number(json, "target_sec", 0.0);
    double prerollSec = json_extract_number(json, "preroll_sec", 0.0);
    if (targetSec < 0.0)
        targetSec = 0.0;
    if (prerollSec < 0.0)
        prerollSec = 0.0;
    double startSec = targetSec - prerollSec;
    if (startSec < 0.0)
        startSec = 0.0;

    OnStopButton();
    SetEditCurPos2(proj, startSec, true, false);
    OnPlayButton();

    char out[256];
    std::snprintf(out, sizeof(out), "OK {\"start_sec\":%.6f,\"target_sec\":%.6f}", startSec, targetSec);
    return std::string(out);
}

//==============================================================
// Import events from MA3
//==============================================================

static std::string handle_import_events(const std::string& json)
{
    ReaProject* proj = EnumProjects(-1, nullptr, 0);
    if (!proj)
        return "ERR NoProject";

    auto events = parse_events(json);
    auto links = load_links(proj);
    int imported = 0;

    for (const auto& ev : events)
    {
        // find link by evId
        std::string guid;
        for (auto& link : links)
        {
            if (link.ma3_evId == ev.evId)
            {
                guid = link.marker_guid;
                link.last_seen = std::time(nullptr);
                link.color_hex = ev.color_hex;
                break;
            }
        }

        int idnum = 0;
        int colorNative = hex_to_native(ev.color_hex);
        if (!guid.empty())
        {
            int colorOld = 0;
            int markerIdx = find_marker_index_by_guid(proj, guid, idnum, colorOld);
            if (markerIdx >= 0)
            {
                SetProjectMarkerByIndex2(proj, markerIdx, false, ev.timeSec, 0.0, idnum, ev.label.c_str(), colorNative, 0);
                imported++;
                continue;
            }
        }

        // create new marker with token
        std::string token = "{ZRS_EV:" + ev.evId + "}";
        int newId = AddProjectMarker2(proj, false, ev.timeSec, 0.0, (ev.label + " " + token).c_str(), -1, colorNative);
        int enumIdx = 0;
        idnum = newId;
        int foundIdx = find_marker_index_by_token(proj, token, idnum);
        if (foundIdx >= 0)
        {
            guid = marker_guid_for_index(proj, foundIdx);
            SetProjectMarkerByIndex2(proj, foundIdx, false, ev.timeSec, 0.0, idnum, ev.label.c_str(), colorNative, 0);
        }
        if (!guid.empty())
        {
            LinkRecord rec;
            rec.marker_guid = guid;
            rec.ma3_evId = ev.evId;
            rec.trackgroup = ev.trackgroup;
            rec.trackno = ev.trackno;
            rec.color_hex = ev.color_hex;
            rec.source = "MA3";
            rec.last_seen = std::time(nullptr);
            links.push_back(rec);
            imported++;
        }
    }

    save_links(proj, links);
    UpdateTimeline();
    return "OK " + std::to_string(imported);
}

//==============================================================
// Networking (reuse simple loop)
//==============================================================

static void handle_client(int client_fd)
{
    std::string buffer;
    char tmp[1024];
    bool exitAfterResponse = false;

    for (;;)
    {
        if (exitAfterResponse)
            break;

        ssize_t n = recv(client_fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
            break;

        buffer.append(tmp, n);

        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);

            line = trim(line);
            if (line.empty())
                continue;

            bool needsResponse = command_needs_response(line);
            if (needsResponse)
            {
                auto cmd = std::make_shared<QueuedCommand>();
                cmd->raw = line;
                cmd->needs_response = true;
                cmd->response = std::make_shared<std::promise<std::string>>();
                auto future = cmd->response->get_future();

                enqueue_command(cmd);

                std::string reply = future.get();
                if (reply.empty())
                    reply = "ERR UnknownCommand";
                reply.push_back('\n');
                send(client_fd, reply.c_str(), reply.size(), 0);

                exitAfterResponse = true;
                break;
            }
            else
            {
                auto cmd = std::make_shared<QueuedCommand>();
                cmd->raw = line;
                enqueue_command(cmd);
            }
        }
    }

    close(client_fd);
}

static void server_thread_fn()
{
    const int PORT = 28731;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return;

    g_listenSock = listen_fd;

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(PORT);

    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(listen_fd);
        g_listenSock = -1;
        return;
    }

    if (listen(listen_fd, 4) < 0)
    {
        close(listen_fd);
        g_listenSock = -1;
        return;
    }

    while (g_serverRunning.load())
    {
        sockaddr_in client_addr;
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
        if (client_fd < 0)
        {
            if (!g_serverRunning.load())
                break;
            continue;
        }
        handle_client(client_fd);
    }

    if (listen_fd >= 0)
        close(listen_fd);

    g_listenSock = -1;
}

static void udp_thread_fn()
{
    const int PORT = 28731;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    g_udpSock = fd;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(PORT);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        g_udpSock = -1;
        return;
    }

    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 200000; // 200ms timeout to allow clean shutdown
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[2048];
    while (g_serverRunning.load())
    {
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n <= 0)
        {
            if (!g_serverRunning.load())
                break;
            continue;
        }

        std::string data(buf, n);
        size_t start = 0;
        for (;;)
        {
            size_t nl = data.find('\n', start);
            std::string line = (nl == std::string::npos)
                                   ? data.substr(start)
                                   : data.substr(start, nl - start);
            line = trim(line);
            if (!line.empty())
            {
                auto cmd = std::make_shared<QueuedCommand>();
                cmd->raw = line;
                enqueue_command(cmd);
            }
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }

    close(fd);
    g_udpSock = -1;
}

static void start_server()
{
    if (g_serverRunning.load())
        return;
    g_serverRunning = true;
    g_serverThread  = std::thread(server_thread_fn);
    g_udpThread     = std::thread(udp_thread_fn);
}

static void stop_server()
{
    if (!g_serverRunning.load())
        return;
    g_serverRunning = false;
    int fd = g_listenSock.load();
    if (fd >= 0)
    {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    int udpFd = g_udpSock.load();
    if (udpFd >= 0)
    {
        shutdown(udpFd, SHUT_RDWR);
        close(udpFd);
    }
    if (g_serverThread.joinable())
        g_serverThread.join();
    if (g_udpThread.joinable())
        g_udpThread.join();
}

//==============================================================
// Timer: process queued commands on main thread
//==============================================================

static void fulfill_response(const std::shared_ptr<QueuedCommand>& req,
                             const std::string& text)
{
    if (!req || !req->needs_response || !req->response)
        return;
    try { req->response->set_value(text); } catch (...) {}
}

static void my_timer()
{
    for (;;)
    {
        std::shared_ptr<QueuedCommand> req;
        {
            std::lock_guard<std::mutex> lock(g_cmdMutex);
            if (g_cmdQueue.empty())
                break;
            req = std::move(g_cmdQueue.front());
            g_cmdQueue.pop_front();
        }

        if (!req) continue;

        std::string raw = req->raw;
        std::string cmd = raw;
        std::string verb = cmd;
        std::string args;
        bool responded = false;
        std::string response;

        size_t sp = cmd.find(' ');
        if (sp != std::string::npos)
        {
            verb = cmd.substr(0, sp);
            args = trim(cmd.substr(sp + 1));
        }

        if (verb == "RS_PING")
        {
            responded = true;
            response = "OK";
        }
        else if (verb == "RS_GET_RESOURCE_PATH")
        {
            responded = true;
            const char* path = GetResourcePath ? GetResourcePath() : "";
            response = std::string("OK ") + (path ? path : "");
        }
        else if (verb == "RS_BUILD_SONG")
        {
            responded = true;
            response = handle_build_song(args);
        }
        else if (verb == "RS_GET_MARKERS_JSON")
        {
            responded = true;
            response = handle_get_markers();
        }
        else if (verb == "RS_GET_PLAYHEAD")
        {
            responded = true;
            response = handle_get_playhead();
        }
        else if (verb == "RS_CREATE_NOTE_MARKER")
        {
            responded = true;
            response = handle_create_note_marker(args);
        }
        else if (verb == "RS_GOTO_AND_PLAY")
        {
            responded = true;
            response = handle_goto_and_play(args);
        }
        else if (verb == "RS_EXTSTATE_GET_ZRS_LINKS")
        {
            responded = true;
            ReaProject* proj = EnumProjects(-1, nullptr, 0);
            if (!proj)
                response = "ERR NoProject";
            else
                response = "OK " + links_to_json(load_links(proj));
        }
        else if (verb == "RS_APPLY_UPDATE")
        {
            responded = true;
            response = handle_apply_update(args);
        }
        else if (verb == "RS_GET_PROJECT_SNAPSHOT")
        {
            responded = true;
            response = handle_get_project_snapshot(args);
        }
        else if (verb == "RS_EXPORT_MIDI_NOTE_EVENTS")
        {
            responded = true;
            response = handle_export_midi_note_events(args);
        }
        else if (verb == "RS_IMPORT_MA3_EVENTS")
        {
            responded = true;
            response = handle_import_events(args);
        }
        else if (verb == "RS_OPEN_PROJECT")
        {
            responded = true;
            response = handle_open_project(args);
        }
        else if (verb == "RS_REORDER_OPEN_PROJECTS")
        {
            responded = true;
            response = handle_reorder_open_projects(args);
        }
        else if (verb == "GetTimeSelectionRange")
        {
            responded = true;
            response = handle_time_selection_range();
        }
        else if (verb == "GetTimeSelection")
        {
            responded = true;
            response = handle_time_selection_length();
        }
        else if (verb == "GotoCue")
        {
            responded = handle_goto_cue(args);
            if (responded)
                response = "OK";
        }
        else if (verb == "PlayFromStart")
        {
            responded = handle_play_from_start(args);
            if (responded)
                response = "OK";
        }
        else if (verb == "ScrubForwards" || verb == "ScrubForward" || verb == "ScrubFwd")
        {
            responded = handle_scrub(args, true);
            if (responded)
                response = "OK";
        }
        else if (verb == "ScrubBackwards" || verb == "ScrubBackward" || verb == "ScrubBack")
        {
            responded = handle_scrub(args, false);
            if (responded)
                response = "OK";
        }
        else if (verb == "ZoomIn")
        {
            responded = handle_zoom(true);
            if (responded)
                response = "OK";
        }
        else if (verb == "ZoomOut")
        {
            responded = handle_zoom(false);
            if (responded)
                response = "OK";
        }
        // existing simple commands preserved
        else if (verb == "Play")
        {
            OnPauseButton();
        }
        else if (verb == "Stop")
        {
            OnStopButton();
            OnPauseButton();
        }
        else
        {
            // Unknown; ignore to avoid popups
        }

        if (req->needs_response)
        {
            if (!responded)
                response = "ERR UnknownCommand";
            fulfill_response(req, response);
        }
    }
}

//==============================================================
// Plugin entrypoint
//==============================================================

extern "C"
REAPER_PLUGIN_DLL_EXPORT
int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                             reaper_plugin_info_t* rec)
{
    if (!rec)
    {
        stop_server();
        if (g_rec)
        {
            g_rec->Register("-timer", (void*)my_timer);
            g_rec = nullptr;
        }
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION)
        return 0;

    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0)
        return 0;

    g_rec = rec;
    g_rec->Register("timer", (void*)my_timer);

    start_server();
    return 1;
}
