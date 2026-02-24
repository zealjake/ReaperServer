// main.cpp - REAPER C++ extension that runs StartFromBeginning
//            about 10 seconds after REAPER has started.

#define REAPERAPI_IMPLEMENT
#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_NamedCommandLookup
#define REAPERAPI_WANT_Main_OnCommand
#define REAPERAPI_WANT_time_precise   // for precise timing

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

static reaper_plugin_info_t* g_rec = nullptr;

// Command ID string for your StartFromBeginning script
static const char* START_FROM_BEGINNING_CMD_STR =
    "_RSb689b0e4bf8952aaa1d59ab77f0b28e5a2eb7a10";

static double g_startTime = 0.0;

// Timer callback – REAPER calls this periodically
static void my_timer()
{
    if (!g_rec) return;

    // Wait until ~10 seconds after start
    double now = time_precise();
    if (now - g_startTime < 10.0)
        return; // too early, keep waiting

    // Only run once
    static bool already_ran = false;
    if (already_ran) return;
    already_ran = true;

    int cmd = NamedCommandLookup(START_FROM_BEGINNING_CMD_STR);
    if (cmd != 0)
    {
        ShowConsoleMsg("MyExt: running StartFromBeginning from extension...\n");
        Main_OnCommand(cmd, 0);
    }
    else
    {
        ShowConsoleMsg("MyExt: could not find StartFromBeginning command.\n");
    }

    // Unregister the timer after running
    g_rec->Register("-timer", (void*)my_timer);
}

extern "C"
REAPER_PLUGIN_DLL_EXPORT
int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                             reaper_plugin_info_t* rec)
{
    // rec == nullptr means plugin is unloading
    if (!rec)
    {
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

    ShowConsoleMsg("MyExt: extension loaded, scheduling StartFromBeginning in 10 seconds...\n");

    // Remember the start time and register the timer
    g_startTime = time_precise();
    g_rec->Register("timer", (void*)my_timer);

    return 1; // success
}
