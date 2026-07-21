/**
 * Test cvxPartBox parameter order
 * This is a minimal test to verify correct parameter usage
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// ZW3D API headers
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\VxApi.h"

// Plugin exports
extern "C" __declspec(dllexport) int TestBoxInit(void);
extern "C" __declspec(dllexport) int TestBoxExit(void);

extern "C" __declspec(dllexport) int TestBoxInit(void)
{
    cvxMsgDisp("=== TestBox Plugin Loaded ===");
    return 0;
}

extern "C" __declspec(dllexport) int TestBoxExit(void)
{
    cvxMsgDisp("=== TestBox Plugin Unloading ===");
    return 0;
}

// Register command function
static int CmdCreateTestBox(void)
{
    cvxMsgDisp("Creating test box with cvxPartBox...");
    char msg[256];

    // Test 1: Standard order (length, width, height, rotation, origin)
    cvxMsgDisp("Test 1: cvxPartBox(100, 60, 40, 0, NULL) - X=100, Y=60, Z=40");
    cvxEntPartHandle h1 = cvxPartBox(100.0, 60.0, 40.0, 0, NULL);
    if (h1) {
        cvxMsgDisp("Test 1: SUCCESS");
    } else {
        cvxMsgDisp("Test 1: FAILED");
    }

    return 0;
}

// Load command
static int g_cmdLoaded = 0;

// This function is called by ZW3D when loading the plugin
// We need to use the proper ZW3D command registration
// Note: This is a simplified test plugin
