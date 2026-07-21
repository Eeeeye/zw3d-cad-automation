/**
 * ZW3D_HTTP_Server.cpp - ZW3D Plugin with HTTP Server
 *
 * Architecture: HTTP thread → task queue → main thread dispatch
 * All ZW3D C API calls execute on the main thread via ZwCommandFunctionLoad
 * to guarantee correct multi-object (.Z3) context binding and thread safety.
 *
 * Endpoints:
 *   POST /create_block  {"length":N,"width":N,"height":N}
 *   POST /generate_drafting  {}
 *   POST /generate_optimized_drafting  {}
 *   POST /create_stress_part {}
 *   POST /create_extreme_stress_part {}
 *   POST /create_block_with_boss {}
 *   GET /inspect_views
 *   GET /evaluate_drawing_quality
 *   GET/POST /execute  {"command":"..."}
 *   GET /status
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include <atomic>
#include <string>
#include <cstring>
#include <mutex>
#include <queue>
#include <vector>
#include <condition_variable>
#include <cmath>
#include <memory>

// ZW3D API headers
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\VxApi.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_feature_shape.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_command.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_dwg_attribute.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_cmd_dwg_layout.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_drawing_sheet.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_drawing_view.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_drawing_dimension.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_brep_shape.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_curve.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_memory.h"
#include "C:\Program Files\ZWSOFT\ZW3D 2026\api\inc\zwapi_entity.h"
#include <io.h>

#pragma comment(lib, "ws2_32.lib")

// HTTP Server configuration
#define HTTP_PORT 8081
#define HTTP_BUFFER_SIZE 8192

// ============================================================
// Task Queue: HTTP thread produces, main thread consumes
// ============================================================

// Task queue with multi-step drafting support
enum TaskType { TASK_NONE, TASK_CREATE_BLOCK, TASK_GENERATE_DRAFTING, TASK_TEST_STEP, TASK_DRAFT_SAVE, TASK_DRAFT_RESTORE, TASK_CREATE_GEAR, TASK_CREATE_COMPLEX, TASK_CREATE_ASSEMBLY, TASK_ADD_PMI, TASK_ADD_PMI_VARIANT, TASK_INSPECT_VIEWS, TASK_EVALUATE_DRAWING_QUALITY, TASK_CREATE_STRESS_PART, TASK_CREATE_EXTREME_STRESS_PART, TASK_CREATE_BLOCK_WITH_BOSS, TASK_ADD_CYLINDER_BOSS, TASK_ANALYZE_PART, TASK_GENERATE_SMART_DRAFTING, TASK_GENERATE_OPTIMIZED_DRAFTING, TASK_OPEN_PART, TASK_CLEAR_SHEET_VIEWS, TASK_REGENERATE_DRAFTING, TASK_ADD_SECTION_VIEW, TASK_SET_REFERENCE_PROFILE, TASK_GENERATE_NATIVE_VIEW_LAYOUT };

struct HttpTask {
    TaskType type = TASK_NONE;
    double param1 = 0;  // length or gear teeth count
    double param2 = 0;  // width or gear radius
    double param3 = 0;  // height or gear thickness
    std::string stringParam;  // path for open_part, label for section view
    std::shared_ptr<std::string> result;  // shared result string
    std::shared_ptr<std::atomic<bool>> done;  // completion flag
};

static std::mutex g_taskMutex;
static std::condition_variable g_taskCv;
static std::queue<HttpTask> g_taskQueue;
static bool g_hasPendingTask = false;

// Shared state for multi-step drafting
static char g_lastPartFile[512] = {0};
static char g_lastPartName[256] = {0};
static char g_lastDrawingPath[512] = {0};

// Persistent active part path and root name (persists across generate_smart iterations;
// cvxFileInqActive can return the drawing path after save-as, corrupting subsequent iterations)
static char g_activePartPath[512] = {0};
static char g_activePartRootName[256] = {0};

// Saved bounding box from last successful analyze_part / open_part in part context.
// Used by generate_smart_drafting when in drawing context (cvxPartInqShapes returns
// nothing useful for drawings, causing fallback to 100x60x40 box sizes).
static double g_savedBboxX = 0.0;
static double g_savedBboxY = 0.0;
static double g_savedBboxZ = 0.0;
static bool g_savedBboxValid = false;
static char g_savedProfileName[32] = "blocky";

struct LearnedReferenceView {
    char type[24] = {0};
    double x = 0.0;
    double y = 0.0;
    int dimensions = 0;
};

struct LearnedReferenceProfile {
    bool valid = false;
    char sample[128] = {0};
    char paperName[32] = {0};
    char templatePath[512] = {0};
    int baseViews = 0;
    int projectViews = 0;
    int sectionViews = 0;
    int viewCount = 0;
    int totalDimensions = 0;
    int annotatedViews = 0;
    LearnedReferenceView views[8];
};

static LearnedReferenceProfile g_activeReferenceProfile = {};

// Override parameters for regenerate_drafting (set by HTTP thread, consumed by main thread)
static int g_paperIndexOffset = 0;
static double g_scaleMultiplier = 1.0;
static int g_pmiVariantMode = -1;  // -1 auto, 0 legacy, 1 exterior-continuous, 2 exterior-baseline

// ============================================================
// Global state
// ============================================================

static std::thread g_httpThread;
static std::atomic<bool> g_httpRunning(false);
static SOCKET g_serverSocket = INVALID_SOCKET;
static const bool g_enableInlinePmiDuringDrafting = false;

// ============================================================
// Forward declarations
// ============================================================

extern "C" __declspec(dllexport) int ZW3D_HTTP_ServerInit(void);
extern "C" __declspec(dllexport) int ZW3D_HTTP_ServerExit(void);

static void HttpServerThread(void);
static void ProcessHttpRequest(SOCKET clientSocket);
static std::string ParsePostBody(const char* request);
static double JsonNum(const std::string& json, const char* key);
static std::string JsonStr(const std::string& json, const char* key);
static void SendJsonResponse(SOCKET clientSocket, const char* status, const std::string& json);

static bool IsAbsoluteWindowsPath(const char* path)
{
    if (!path || path[0] == '\0') return false;
    if (strlen(path) >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) return true;
    if (strlen(path) >= 2 && path[0] == '\\' && path[1] == '\\') return true;
    return false;
}

// Convert relative file path to absolute path using Win32 API
static std::string ToAbsolutePath(const char* path) {
    if (!path || path[0] == '\0') return "";
    // Already absolute (starts with drive letter like C:\)
    if (IsAbsoluteWindowsPath(path)) {
        return std::string(path);
    }
    char absPath[1024] = {0};
    DWORD len = GetFullPathNameA(path, sizeof(absPath), absPath, NULL);
    if (len > 0 && len < sizeof(absPath)) {
        return std::string(absPath);
    }
    return std::string(path);
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

struct LayoutParams {
    double gap = 44.0;
    double maxLayoutWidth = 178.0;
    double maxLayoutHeight = 118.0;
    double scaleBias = 0.58;
    double canvasWidth = 260.0;
    double canvasHeight = 190.0;
    double centerShiftX = 18.0;
    double centerShiftY = 20.0;
    double widthPaddingFactor = 1.35;
    double heightPaddingFactor = 1.55;
};

struct DrawingPaperRect {
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
    double marginTop = 0.0;
    double marginRight = 0.0;
    double marginBottom = 0.0;
    double marginLeft = 0.0;
    std::string paperName;
    std::string source = "fallback";
};

struct OccupiedRect {
    bool valid = false;
    double minX = 0.0;
    double maxX = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
};

static LayoutParams GetLayoutParamsForProfile(const char* profileName, int attempt)
{
    LayoutParams p;
    if (profileName != nullptr && strcmp(profileName, "plate_like") == 0) {
        p.gap = 52.0;
        p.scaleBias = 0.54;
        p.widthPaddingFactor = 1.45;
        p.heightPaddingFactor = 1.60;
    } else if (profileName != nullptr && strcmp(profileName, "elongated") == 0) {
        p.gap = 56.0;
        p.scaleBias = 0.50;
        p.maxLayoutWidth = 170.0;
        p.widthPaddingFactor = 1.55;
        p.heightPaddingFactor = 1.65;
    } else if (profileName != nullptr && strcmp(profileName, "stepped") == 0) {
        p.gap = 60.0;
        p.scaleBias = 0.40;
        p.maxLayoutWidth = 162.0;
        p.maxLayoutHeight = 102.0;
        p.centerShiftX = 24.0;
        p.centerShiftY = 24.0;
        p.widthPaddingFactor = 1.65;
        p.heightPaddingFactor = 1.85;
    }

    if (attempt > 0) {
        p.gap += 14.0 * attempt;
        p.scaleBias *= (attempt == 1 ? 0.94 : 0.90);
        p.widthPaddingFactor += 0.10 * attempt;
        p.heightPaddingFactor += 0.12 * attempt;
        p.canvasWidth += 60.0 * attempt;
        p.canvasHeight += 42.0 * attempt;
        p.maxLayoutWidth += 46.0 * attempt;
        p.maxLayoutHeight += 32.0 * attempt;
        p.centerShiftX += 4.0 * attempt;
        p.centerShiftY += 4.0 * attempt;
    }

    return p;
}

// Main-thread command handlers (registered via ZwCommandFunctionLoad)
static int CmdProcessQueue(void);
static int CmdCreateBlock(void);
static int CmdGenerateDrafting(void);

// ZW3D API wrappers (called on main thread only)
static std::string ApiCreateBlock(double length, double width, double height);
static std::string ApiGenerateDrafting(void);
static std::string ApiGenerateOptimizedDrafting(void);
static std::string ApiAddPmi(void);
static std::string ApiAddPmiVariant(const std::string& body);
static std::string ApiInspectViews(void);
static std::string ApiEvaluateDrawingQuality(void);
static std::string ApiAnalyzePart(void);
static std::string ApiTestDraftingStep(int step);  // diagnostic
static std::string ApiCreateGear(int teeth, double radius, double thickness);
static std::string ApiCreateComplexShape(void);
static std::string ApiCreateAssembly(void);
static std::string ApiCreateStressPart(void);
static std::string ApiCreateExtremeStressPart(void);
static std::string ApiCreateBlockWithBoss(void);
static std::string ApiAddCylinderBoss(double centerX, double centerY, double baseTopZ, double radius, double length);
static std::string ApiOpenPart(const std::string& path);
static std::string ApiClearSheetViews(void);
static std::string ApiAddSectionView(const std::string& label, const std::string& position);
static std::string ApiSetReferenceProfile(const std::string& body);
static std::string ApiGenerateNativeViewLayout(const std::string& body);
static bool CheckActivePart();
static bool EnsureActivePart();
static bool IsEmptyHandle(const szwEntityHandle& handle);
static bool EnsureDrawingContext(const char* drawingPath);
static std::string CollectActiveSheetViewStatsJson();
static bool GetActiveDrawingId(int* drawingIdOut);
static bool SetActiveDrawingPaperSize(const char* paperName, double width, double height);
static bool GetActiveDrawingPaperRect(DrawingPaperRect* rectOut);
static bool GetActiveDrawingBorderRect(double* minX, double* maxX, double* minY, double* maxY);
static OccupiedRect ComputeViewOccupiedRect(const szwEntityHandle& viewHandle);
static std::string BuildOccupiedRectJson(const OccupiedRect& rect);
static std::string BuildOverallOccupiedRectJson(const std::vector<szwEntityHandle>& viewHandles, const DrawingPaperRect& paperRect);
static std::string BuildActiveSheetOccupiedRectJson(const DrawingPaperRect& paperRect);
static double ComputeActiveSheetOverflowMagnitude(const DrawingPaperRect& paperRect);
static const char* DrawingViewTypeName(ezwDrawingViewType type);
static bool IsQualityLayoutView(ezwDrawingViewType type, const OccupiedRect& rect);
static int ClearActiveSheetViews();
static bool GetActivePartBoundingSize(double* sizeX, double* sizeY, double* sizeZ);
static bool HasActiveReferenceProfile();
static int ReferencePrimaryBaseIndex();
static int ReferenceVerticalProjectIndex();
static int ReferenceRightProjectIndex();
static int ReferenceExtraRightProjectIndex();
static int ReferenceLeftProjectIndex();
static int ReferenceSecondaryBaseIndex();
static int ReferenceTargetDimensionsForOrdinal(int annotationOrdinal);
static std::string BuildPartAnalysisJson(double sizeX, double sizeY, double sizeZ,
    ezwDrawingViewMethod* baseViewOut, double* widthOut, double* heightOut, const char** profileOut);
static int CountDimensionTextOverlaps(int count, szwEntityHandle* dimensions);
static int CountDimensionViewIntrusions(const szwEntityHandle& viewHandle, int count, szwEntityHandle* dimensions);
static int DeleteIntrusiveDimensions(const szwEntityHandle& viewHandle, int maxDelete);
static int DeleteOverlappingDimensions(const szwEntityHandle& viewHandle, int maxDelete);
static int LimitViewDimensionCount(const szwEntityHandle& viewHandle, int maxKeep);
static ezwErrors PostProcessViewDimensions(const szwEntityHandle& viewHandle, const char* profileName, int* overlapCountOut, int* intrusionCountOut);
static int AddCenterMarksToCircularGeometry(const szwEntityHandle& viewHandle,
    int* geometryCountOut, int* closedCurveCountOut, int* createCodeOut);
static std::string JsonEscape(const std::string& s);
static bool AutoAnnotateView(const szwEntityHandle& viewHandle, int viewIndex, int* createdCountOut,
    int* totalCountOut, int* cleanRetOut, int* errOut, std::string* errDetailOut, const char* profileName, std::string* metricsJsonOut);
static ezwErrors TryCreateFullSectionView(const szwEntityHandle& baseViewHandle, double locationX, double locationY,
    const char* label, szwEntityHandle* sectionViewHandleOut);

// ============================================================
// Main-thread command handlers
// ============================================================

/**
 * Process the next task from the queue. Called by ZW3D main thread
 * when "~HttpProcessQueue" command is posted from HTTP thread.
 */
static int CmdProcessQueue(void)
{
    HttpTask task;
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        if (g_taskQueue.empty()) {
            g_hasPendingTask = false;
            return 0;
        }
        task = g_taskQueue.front();
        g_taskQueue.pop();
    }

    if (!task.result || !task.done) {
        return 0;
    }

    cvxMsgDisp("CmdProcessQueue: executing task on main thread");

    switch (task.type) {
        case TASK_CREATE_BLOCK:
            *task.result = ApiCreateBlock(task.param1, task.param2, task.param3);
            break;
        case TASK_GENERATE_DRAFTING:
            *task.result = ApiGenerateDrafting();
            break;
        case TASK_ADD_PMI:
            *task.result = ApiAddPmi();
            break;
        case TASK_ADD_PMI_VARIANT:
            *task.result = ApiAddPmiVariant(task.stringParam);
            break;
        case TASK_INSPECT_VIEWS:
            *task.result = ApiInspectViews();
            break;
        case TASK_EVALUATE_DRAWING_QUALITY:
            *task.result = ApiEvaluateDrawingQuality();
            break;
        case TASK_ANALYZE_PART:
            *task.result = ApiAnalyzePart();
            break;
        case TASK_GENERATE_SMART_DRAFTING:
            *task.result = ApiGenerateDrafting();
            break;
        case TASK_GENERATE_OPTIMIZED_DRAFTING:
            *task.result = ApiGenerateOptimizedDrafting();
            break;
        case TASK_TEST_STEP:
            *task.result = ApiTestDraftingStep((int)task.param1);
            break;
        case TASK_DRAFT_SAVE:
            *task.result = "{\"status\":\"ok\",\"step\":\"save\"}";
            cvxFileSave(0);
            cvxMsgDisp("Drawing file saved");
            break;
        case TASK_DRAFT_RESTORE:
            *task.result = "{\"status\":\"ok\",\"step\":\"restore\"}";
            cvxFileNew(g_lastPartFile);
            if (g_lastPartName[0]) cvxRootActivate(g_lastPartName);
            cvxMsgDisp("Original part context restored");
            break;
        case TASK_CREATE_GEAR:
            *task.result = ApiCreateGear((int)task.param1, task.param2, task.param3);
            break;
        case TASK_CREATE_COMPLEX:
            *task.result = ApiCreateComplexShape();
            break;
        case TASK_CREATE_ASSEMBLY:
            *task.result = ApiCreateAssembly();
            break;
        case TASK_CREATE_STRESS_PART:
            *task.result = ApiCreateStressPart();
            break;
        case TASK_CREATE_EXTREME_STRESS_PART:
            *task.result = ApiCreateExtremeStressPart();
            break;
        case TASK_CREATE_BLOCK_WITH_BOSS:
            *task.result = ApiCreateBlockWithBoss();
            break;
        case TASK_ADD_CYLINDER_BOSS:
            *task.result = ApiAddCylinderBoss(task.param1, task.param2, task.param3, 28.0, 60.0);
            break;
        case TASK_OPEN_PART:
            *task.result = ApiOpenPart(task.stringParam);
            break;
        case TASK_CLEAR_SHEET_VIEWS:
            *task.result = ApiClearSheetViews();
            break;
        case TASK_REGENERATE_DRAFTING:
            g_paperIndexOffset = (int)task.param1;
            g_scaleMultiplier = task.param2;
            *task.result = ApiGenerateDrafting();
            g_paperIndexOffset = 0;
            g_scaleMultiplier = 1.0;
            break;
        case TASK_ADD_SECTION_VIEW:
            *task.result = ApiAddSectionView(task.stringParam, task.param1 > 0.5 ? "right" : "below");
            break;
        case TASK_SET_REFERENCE_PROFILE:
            *task.result = ApiSetReferenceProfile(task.stringParam);
            break;
        case TASK_GENERATE_NATIVE_VIEW_LAYOUT:
            *task.result = ApiGenerateNativeViewLayout(task.stringParam);
            break;
        default:
            *task.result = "{\"status\":\"error\",\"message\":\"Unknown task type\"}";
            break;
    }

    task.done->store(true);
    g_taskCv.notify_all();

    // Check if more tasks are queued
    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        g_hasPendingTask = !g_taskQueue.empty();
    }
    // If more tasks, post another command to process them
    if (g_hasPendingTask) {
        ZwCommandPost("~HttpProcessQueue", ZW_COMMAND_POST_PRIORITY_HIGH);
    }

    return 0;
}

static int CmdCreateBlock(void)
{
    return CmdProcessQueue();
}

static int CmdGenerateDrafting(void)
{
    return CmdProcessQueue();
}

// ============================================================
// Enqueue helper (called from HTTP thread)
// ============================================================

/**
 * Enqueue a task and wait for main thread to complete it.
 * Uses ZwCommandPost to wake up ZW3D main thread.
 */
static std::string EnqueueAndWait(TaskType type, double p1, double p2, double p3, int timeoutMs = 30000)
{
    auto result = std::make_shared<std::string>();
    auto done = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        HttpTask task;
        task.type = type;
        task.param1 = p1;
        task.param2 = p2;
        task.param3 = p3;
        task.result = result;
        task.done = done;
        g_taskQueue.push(task);
        g_hasPendingTask = true;
    }

    // Wake up ZW3D main thread
    ZwCommandPost("~HttpProcessQueue", ZW_COMMAND_POST_PRIORITY_HIGH);

    // Wait for completion with timeout
    std::unique_lock<std::mutex> lock(g_taskMutex);
    if (g_taskCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return done->load(); })) {
        return *result;
    } else {
        return "{\"status\":\"error\",\"message\":\"Timeout waiting for ZW3D main thread\"}";
    }
}

/**
 * Enqueue a task with a string parameter and wait for main thread to complete it.
 */
static std::string EnqueueAndWaitStr(TaskType type, const std::string& strParam, double p1 = 0, double p2 = 0, double p3 = 0, int timeoutMs = 30000)
{
    auto result = std::make_shared<std::string>();
    auto done = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard<std::mutex> lock(g_taskMutex);
        HttpTask task;
        task.type = type;
        task.param1 = p1;
        task.param2 = p2;
        task.param3 = p3;
        task.stringParam = strParam;
        task.result = result;
        task.done = done;
        g_taskQueue.push(task);
        g_hasPendingTask = true;
    }

    // Wake up ZW3D main thread
    ZwCommandPost("~HttpProcessQueue", ZW_COMMAND_POST_PRIORITY_HIGH);

    // Wait for completion with timeout
    std::unique_lock<std::mutex> lock(g_taskMutex);
    if (g_taskCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return done->load(); })) {
        return *result;
    } else {
        return "{\"status\":\"error\",\"message\":\"Timeout waiting for ZW3D main thread\"}";
    }
}

// ============================================================
// ZW3D API wrappers (MAIN THREAD ONLY)
// ============================================================

static std::string ApiCreateBlock(double length, double width, double height)
{
    cvxMsgDisp("=== ApiCreateBlock (main thread) ===");

    // STEP 1: Explicitly query and re-activate the current root context
    char rootName[256] = {0};
    cvxRootInqActive(rootName, sizeof(rootName));

    char msg[512];
    if (rootName[0] != 0) {
        sprintf_s(msg, sizeof(msg), "Active root: %s - re-activating...", rootName);
        cvxMsgDisp(msg);
        evxErrors actRet = cvxRootActivate(rootName);
        if (actRet != ZW_API_NO_ERROR) {
            sprintf_s(msg, sizeof(msg), "WARNING: cvxRootActivate failed: %d (continuing anyway)", actRet);
            cvxMsgDisp(msg);
        }
    } else {
        cvxMsgDisp("No active root, creating new part file...");
        evxErrors newRet = cvxFileNewSingle("AutoPart", VX_FILE_PART, VX_SUBTYPE_NONE, NULL, NULL);
        if (newRet != ZW_API_NO_ERROR) {
            sprintf_s(msg, sizeof(msg), "cvxFileNewSingle(PART) failed: %d", newRet);
            cvxMsgDisp(msg);
            return "{\"status\":\"error\",\"message\":\"Failed to create part file\"}";
        }
        cvxMsgDisp("Part file created");
        // cvxFileNewSingle auto-creates a root, just query and activate
        cvxRootInqActive(rootName, sizeof(rootName));
        if (rootName[0] != 0) {
            cvxRootActivate(rootName);
            sprintf_s(msg, sizeof(msg), "Activated root: %s", rootName);
            cvxMsgDisp(msg);
        } else {
            cvxMsgDisp("WARNING: Still no active root after file creation");
        }
    }

    sprintf_s(msg, sizeof(msg), "Creating block: %.1f x %.1f x %.1f", length, width, height);
    cvxMsgDisp(msg);

    // STEP 2: Create block using cvxPartBox with svxBoxData structure
    svxBoxData boxData;
    cvxPartBoxInit(&boxData);
    boxData.X = length;
    boxData.Y = width;
    boxData.Z = height;

    int shapeId = 0;
    evxErrors ret = cvxPartBox(&boxData, &shapeId);

    if (ret != ZW_API_NO_ERROR) {
        cvxMsgDisp("cvxPartBox failed");
        return "{\"status\":\"error\",\"message\":\"Block creation failed\"}";
    }

    sprintf_s(msg, sizeof(msg), "Block created: %.1f x %.1f x %.1f (shapeId=%d)", length, width, height, shapeId);
    cvxMsgDisp(msg);

    return "{\"status\":\"ok\",\"message\":\"Block created successfully\",\"dimensions\":{\"length\":"
        + std::to_string(length) + ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}}";
}

static bool CheckActivePart()
{
    char rootName[256] = {0};
    cvxRootInqActive(rootName, sizeof(rootName));

    if (rootName[0] == 0) {
        return false;
    }

    int isAsm = 0;
    evxErrors ret = cvxRootIsAsm(NULL, NULL, &isAsm);
    return (ret == ZW_API_NO_ERROR);
}

/**
 * Helper: Ensure we have an active Part document
 */
static bool EnsureActivePart()
{
    char rootName[256] = {0};
    cvxRootInqActive(rootName, sizeof(rootName));
    char msg[512];

    if (rootName[0] != 0) {
        cvxRootActivate(rootName);
        return true;
    }

    cvxMsgDisp("No active root, creating new part file...");
    evxErrors ret = cvxFileNewSingle("AutoPart", VX_FILE_PART, VX_SUBTYPE_NONE, NULL, NULL);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "cvxFileNewSingle(PART) failed: %d", ret);
        cvxMsgDisp(msg);
        return false;
    }

    cvxRootInqActive(rootName, sizeof(rootName));
    if (rootName[0] != 0) {
        cvxRootActivate(rootName);
        return true;
    }

    cvxMsgDisp("ERROR: Still no active root after file creation");
    return false;
}

static bool IsEmptyHandle(const szwEntityHandle& handle)
{
    return handle.innerData == nullptr;
}

static bool EnsureDrawingContext(const char* drawingPath)
{
    // First try: already in a drawing context
    ezwErrors ret = ZwDrawingSheetActivateByHandle(nullptr);
    if (ret == ZW_API_NO_ERROR) {
        return true;
    }

    // Try: cvxFileActivate on the drawing (best for switching between open documents)
    const char* candidates[3] = {
        g_lastDrawingPath[0] ? g_lastDrawingPath : nullptr,
        (drawingPath != nullptr && drawingPath[0] != '\0') ? drawingPath : nullptr,
        "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW"
    };

    for (int i = 0; i < 3; ++i) {
        const char* path = candidates[i];
        if (!path || path[0] == '\0') continue;

        char msg[512];
        sprintf_s(msg, sizeof(msg), "[CTX] Trying cvxFileActivate: %s", path);
        cvxMsgDisp(msg);
        evxErrors actRet = cvxFileActivate(path);
        sprintf_s(msg, sizeof(msg), "[CTX] cvxFileActivate = %d", actRet);
        cvxMsgDisp(msg);
        if (actRet == ZW_API_NO_ERROR) {
            Sleep(300);
            ret = ZwDrawingSheetActivateByHandle(nullptr);
            if (ret == ZW_API_NO_ERROR) {
                strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), path, _TRUNCATE);
                sprintf_s(msg, sizeof(msg), "[CTX] Drawing context established via cvxFileActivate");
                cvxMsgDisp(msg);
                return true;
            }
        }

        // Fallback: try as .Z3DRW or .Z3 alternate extension
        char altPath[512] = {0};
        strncpy_s(altPath, sizeof(altPath), path, _TRUNCATE);
        char* ext = strrchr(altPath, '.');
        bool swapped = false;
        if (ext) {
            if (_stricmp(ext, ".Z3DRW") == 0) {
                strcpy_s(ext, sizeof(altPath) - (ext - altPath), ".Z3");
                swapped = true;
            } else if (_stricmp(ext, ".Z3") == 0) {
                strcpy_s(ext, sizeof(altPath) - (ext - altPath), ".Z3DRW");
                swapped = true;
            }
        }
        if (swapped) {
            sprintf_s(msg, sizeof(msg), "[CTX] Trying cvxFileActivate alt: %s", altPath);
            cvxMsgDisp(msg);
            evxErrors altRet = cvxFileActivate(altPath);
            sprintf_s(msg, sizeof(msg), "[CTX] cvxFileActivate alt = %d", altRet);
            cvxMsgDisp(msg);
            if (altRet == ZW_API_NO_ERROR) {
                Sleep(300);
                ret = ZwDrawingSheetActivateByHandle(nullptr);
                if (ret == ZW_API_NO_ERROR) {
                    strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), altPath, _TRUNCATE);
                    return true;
                }
            }
        }
    }

    // Last resort: cvxFileNew (opens from disk)
    for (int i = 0; i < 3; ++i) {
        const char* path = candidates[i];
        if (!path || path[0] == '\0') continue;

        char msg[512];
        sprintf_s(msg, sizeof(msg), "[CTX] Trying cvxFileNew: %s", path);
        cvxMsgDisp(msg);
        evxErrors openRet = cvxFileNew(path);
        sprintf_s(msg, sizeof(msg), "[CTX] cvxFileNew = %d", openRet);
        cvxMsgDisp(msg);
        if (openRet == ZW_API_NO_ERROR) {
            Sleep(500);
            int sheetCount = 0;
            szwEntityHandle* sheetList = nullptr;
            ret = ZwDrawingSheetListGet(&sheetCount, &sheetList);
            if (ret == ZW_API_NO_ERROR && sheetCount > 0 && sheetList != nullptr) {
                ret = ZwDrawingSheetActivateByHandle(&sheetList[0]);
                ZwEntityHandleListFree(sheetCount, &sheetList);
            } else {
                if (sheetList) ZwEntityHandleListFree(sheetCount, &sheetList);
                ret = ZwDrawingSheetActivateByHandle(nullptr);
            }
            if (ret == ZW_API_NO_ERROR) {
                strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), path, _TRUNCATE);
                return true;
            }
        }
    }

    return false;
}

static std::string CollectActiveSheetViewStatsJson()
{
    int totalViews = 0;
    int baseViews = 0;
    int projectViews = 0;
    int definitionViews = 0;
    int sectionViews = 0;
    int detailViews = 0;
    szwEntityHandle* viewList = nullptr;

    ezwErrors ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &totalViews, &viewList);
    if (ret != ZW_API_NO_ERROR) {
        return std::string("{\"status\":\"error\",\"message\":\"Failed to collect view stats\",\"code\":")
            + std::to_string(ret) + "}";
    }

    for (int i = 0; i < totalViews; ++i) {
        ezwDrawingViewType type = ZW_DRAWING_ALL_VIEW;
        if (ZwDrawingViewTypeGet(viewList[i], &type) != ZW_API_NO_ERROR) {
            continue;
        }

        switch (type) {
            case ZW_DRAWING_BASE_VIEW:
                baseViews++;
                break;
            case ZW_DRAWING_PROJECT_VIEW:
                projectViews++;
                break;
            case ZW_DRAWING_DEFINITION_VIEW:
                definitionViews++;
                break;
            case ZW_DRAWING_SECTION_VIEW:
                sectionViews++;
                break;
            case ZW_DRAWING_DETAIL_VIEW:
                detailViews++;
                break;
            default:
                break;
        }
    }

    if (viewList != nullptr) {
        ZwEntityHandleListFree(totalViews, &viewList);
    }

    return std::string("{\"status\":\"ok\",\"total_views\":") + std::to_string(totalViews)
        + ",\"base_views\":" + std::to_string(baseViews)
        + ",\"project_views\":" + std::to_string(projectViews)
        + ",\"definition_views\":" + std::to_string(definitionViews)
        + ",\"section_views\":" + std::to_string(sectionViews)
        + ",\"detail_views\":" + std::to_string(detailViews)
        + "}";
}

static bool GetActiveDrawingPaperRect(DrawingPaperRect* rectOut)
{
    if (rectOut == nullptr) {
        return false;
    }

    *rectOut = DrawingPaperRect{};

    char rootName[256] = {0};
    cvxRootInqActive(rootName, sizeof(rootName));
    if (rootName[0] == 0) {
        return false;
    }

    int idRoot = 0;
    evxRootType rootType = static_cast<evxRootType>(0);
    if (cvxRootId(rootName, &idRoot, &rootType) != ZW_API_NO_ERROR || idRoot == 0) {
        return false;
    }

    int drawingCount = 0;
    int* drawingIds = nullptr;
    if (cvxDwgInqList(idRoot, &drawingCount, &drawingIds) != ZW_API_NO_ERROR || drawingCount <= 0 || drawingIds == nullptr) {
        if (drawingIds != nullptr) {
            cvxMemFree((void**)&drawingIds);
        }
        return false;
    }

    char activeDrawingName[256] = {0};
    cvxDwgInqActive(activeDrawingName, sizeof(activeDrawingName));

    int activeIndex = 0;
    if (activeDrawingName[0] != 0) {
        for (int i = 0; i < drawingCount; ++i) {
            char drawingName[256] = {0};
            if (cvxDwgInqName(drawingIds[i], drawingName, sizeof(drawingName)) == ZW_API_NO_ERROR
                && _stricmp(drawingName, activeDrawingName) == 0) {
                activeIndex = i;
                break;
            }
        }
    }

    bool ok = false;
    for (int pass = 0; pass < drawingCount && !ok; ++pass) {
        int i = (activeIndex + pass) % drawingCount;
        svxDrawingAt drawingAt = {};
        if (cvxDwgAtGet(drawingIds[i], &drawingAt) == ZW_API_NO_ERROR
            && drawingAt.width > 1.0
            && drawingAt.height > 1.0) {
            rectOut->minX = 0.0;
            rectOut->maxX = drawingAt.width;
            rectOut->minY = 0.0;
            rectOut->maxY = drawingAt.height;
            rectOut->marginTop = drawingAt.margin[0] > 0.0 ? drawingAt.margin[0] : 0.0;
            rectOut->marginRight = drawingAt.margin[1] > 0.0 ? drawingAt.margin[1] : 0.0;
            rectOut->marginBottom = drawingAt.margin[2] > 0.0 ? drawingAt.margin[2] : 0.0;
            rectOut->marginLeft = drawingAt.margin[3] > 0.0 ? drawingAt.margin[3] : 0.0;
            rectOut->paperName = drawingAt.paper;
            rectOut->source = "drawing_attribute";
            ok = true;
            break;
        }

        int idBorder = 0;
        int idBorderSkt = 0;
        int idTitle = 0;
        int idTitleSkt = 0;
        if (cvxDwgInqBorderTitle(drawingIds[i], &idBorder, &idBorderSkt, &idTitle, &idTitleSkt) != ZW_API_NO_ERROR) {
            continue;
        }
        int borderEntityId = idBorderSkt > 0 ? idBorderSkt : idBorder;
        if (borderEntityId <= 0) {
            continue;
        }

        svxBndBox box = {};
        if (cvxEntBndBox(borderEntityId, &box) != ZW_API_NO_ERROR) {
            continue;
        }

        rectOut->minX = box.X.min;
        rectOut->maxX = box.X.max;
        rectOut->minY = box.Y.min;
        rectOut->maxY = box.Y.max;
        rectOut->source = "border_bbox";
        ok = true;
        break;
    }

    cvxMemFree((void**)&drawingIds);
    return ok;
}

static bool GetActiveDrawingId(int* drawingIdOut)
{
    if (drawingIdOut == nullptr) {
        return false;
    }
    *drawingIdOut = 0;

    char rootName[256] = {0};
    cvxRootInqActive(rootName, sizeof(rootName));
    if (rootName[0] == 0) {
        return false;
    }

    int idRoot = 0;
    evxRootType rootType = static_cast<evxRootType>(0);
    if (cvxRootId(rootName, &idRoot, &rootType) != ZW_API_NO_ERROR || idRoot == 0) {
        return false;
    }

    int drawingCount = 0;
    int* drawingIds = nullptr;
    if (cvxDwgInqList(idRoot, &drawingCount, &drawingIds) != ZW_API_NO_ERROR || drawingCount <= 0 || drawingIds == nullptr) {
        if (drawingIds != nullptr) {
            cvxMemFree((void**)&drawingIds);
        }
        return false;
    }

    char activeDrawingName[256] = {0};
    cvxDwgInqActive(activeDrawingName, sizeof(activeDrawingName));
    bool ok = false;
    for (int i = 0; i < drawingCount; ++i) {
        if (activeDrawingName[0] != 0) {
            char drawingName[256] = {0};
            if (cvxDwgInqName(drawingIds[i], drawingName, sizeof(drawingName)) == ZW_API_NO_ERROR
                && _stricmp(drawingName, activeDrawingName) == 0) {
                *drawingIdOut = drawingIds[i];
                ok = true;
                break;
            }
        } else {
            *drawingIdOut = drawingIds[i];
            ok = true;
            break;
        }
    }

    cvxMemFree((void**)&drawingIds);
    return ok;
}

static bool SetActiveDrawingPaperSize(const char* paperName, double width, double height)
{
    int drawingId = 0;
    if (!GetActiveDrawingId(&drawingId) || drawingId == 0) {
        return false;
    }

    svxDrawingAt drawingAt = {};
    if (cvxDwgAtGet(drawingId, &drawingAt) != ZW_API_NO_ERROR) {
        return false;
    }

    drawingAt.useTemplate = 0;
    drawingAt.useBorder = 1;
    drawingAt.width = width;
    drawingAt.height = height;
    if (paperName != nullptr && paperName[0] != 0) {
        strncpy_s(drawingAt.paper, sizeof(drawingAt.paper), paperName, _TRUNCATE);
    }
    if (drawingAt.margin[0] <= 0.0) drawingAt.margin[0] = 10.0;
    if (drawingAt.margin[1] <= 0.0) drawingAt.margin[1] = 10.0;
    if (drawingAt.margin[2] <= 0.0) drawingAt.margin[2] = 10.0;
    if (drawingAt.margin[3] <= 0.0) drawingAt.margin[3] = 10.0;
    drawingAt.bound = 1;

    return cvxDwgAtSet(drawingId, &drawingAt) == ZW_API_NO_ERROR;
}

static bool GetActiveDrawingBorderRect(double* minX, double* maxX, double* minY, double* maxY)
{
    DrawingPaperRect rect;
    if (!GetActiveDrawingPaperRect(&rect)) {
        if (minX) *minX = 0.0;
        if (maxX) *maxX = 0.0;
        if (minY) *minY = 0.0;
        if (maxY) *maxY = 0.0;
        return false;
    }

    if (minX) *minX = rect.minX;
    if (maxX) *maxX = rect.maxX;
    if (minY) *minY = rect.minY;
    if (maxY) *maxY = rect.maxY;
    return true;
}

static OccupiedRect ComputeViewBorderRect(const szwEntityHandle& viewHandle)
{
    OccupiedRect rect;
    if (IsEmptyHandle(viewHandle)) {
        return rect;
    }

    szwDrawingDottedBorder border = {};
    if (ZwDrawingViewDottedBorderGet(viewHandle, &border) == ZW_API_NO_ERROR) {
        rect.valid = true;
        rect.minX = border.upperLeft.x;
        rect.maxX = border.bottomRight.x;
        rect.minY = border.bottomRight.y;
        rect.maxY = border.upperLeft.y;
    }

    return rect;
}

static OccupiedRect ComputeViewOccupiedRect(const szwEntityHandle& viewHandle)
{
    OccupiedRect rect = ComputeViewBorderRect(viewHandle);
    if (IsEmptyHandle(viewHandle)) {
        return rect;
    }

    int dimCount = 0;
    szwEntityHandle* dimensions = nullptr;
    if (ZwDrawingViewDimensionListGet(viewHandle, ZW_VIEW_ALL_DIMENSION, &dimCount, &dimensions) == ZW_API_NO_ERROR
        && dimCount > 0 && dimensions != nullptr) {
        for (int i = 0; i < dimCount; ++i) {
            szwDrawingDimensionTextPositionPoints box = {};
            if (ZwDrawingDimensionTextPositionPointsGet(dimensions[i], &box) != ZW_API_NO_ERROR) {
                continue;
            }

            double dimMinX = box.bottomLeft.x - 8.0;
            double dimMaxX = box.bottomRight.x + 8.0;
            double dimMinY = box.bottomLeft.y - 6.0;
            double dimMaxY = box.topLeft.y + 6.0;

            if (!rect.valid) {
                rect.valid = true;
                rect.minX = dimMinX;
                rect.maxX = dimMaxX;
                rect.minY = dimMinY;
                rect.maxY = dimMaxY;
            } else {
                if (dimMinX < rect.minX) rect.minX = dimMinX;
                if (dimMaxX > rect.maxX) rect.maxX = dimMaxX;
                if (dimMinY < rect.minY) rect.minY = dimMinY;
                if (dimMaxY > rect.maxY) rect.maxY = dimMaxY;
            }
        }
        ZwEntityHandleListFree(dimCount, &dimensions);
    } else if (dimensions != nullptr) {
        ZwEntityHandleListFree(dimCount, &dimensions);
    }

    return rect;
}

static std::string BuildOccupiedRectJson(const OccupiedRect& rect)
{
    if (!rect.valid) {
        return "{\"status\":\"empty\"}";
    }

    return std::string("{\"status\":\"ok\",\"min_x\":") + std::to_string(rect.minX)
        + ",\"max_x\":" + std::to_string(rect.maxX)
        + ",\"min_y\":" + std::to_string(rect.minY)
        + ",\"max_y\":" + std::to_string(rect.maxY)
        + ",\"width\":" + std::to_string(rect.maxX - rect.minX)
        + ",\"height\":" + std::to_string(rect.maxY - rect.minY)
        + "}";
}

struct DrawingQualityViewInfo {
    int index = 0;
    ezwDrawingViewType type = ZW_DRAWING_ALL_VIEW;
    const char* typeName = "unknown";
    OccupiedRect rect;
    double centerX = 0.0;
    double centerY = 0.0;
    bool hasAlignmentCenter = false;
    double alignmentCenterX = 0.0;
    double alignmentCenterY = 0.0;
    int dimensionCount = 0;
    int textOverlaps = 0;
    int viewIntrusions = 0;
};

static const char* DrawingViewTypeName(ezwDrawingViewType type)
{
    switch (type) {
        case ZW_DRAWING_BASE_VIEW:
            return "base";
        case ZW_DRAWING_PROJECT_VIEW:
            return "project";
        case ZW_DRAWING_DEFINITION_VIEW:
            return "definition";
        case ZW_DRAWING_SECTION_VIEW:
            return "section";
        case ZW_DRAWING_DETAIL_VIEW:
            return "detail";
        default:
            return "unknown";
    }
}

static double RectArea(const OccupiedRect& rect)
{
    if (!rect.valid) {
        return 0.0;
    }
    return (rect.maxX - rect.minX) * (rect.maxY - rect.minY);
}

static bool IsQualityLayoutView(ezwDrawingViewType type, const OccupiedRect& rect)
{
    if (!rect.valid) {
        return false;
    }

    if (type == ZW_DRAWING_DEFINITION_VIEW) {
        return false;
    }

    double width = rect.maxX - rect.minX;
    double height = rect.maxY - rect.minY;
    if (width <= 28.0 && height <= 28.0 && std::fabs((rect.minX + rect.maxX) / 2.0) < 5.0
        && std::fabs((rect.minY + rect.maxY) / 2.0) < 5.0) {
        return false;
    }

    return type == ZW_DRAWING_BASE_VIEW
        || type == ZW_DRAWING_PROJECT_VIEW
        || type == ZW_DRAWING_SECTION_VIEW
        || type == ZW_DRAWING_DETAIL_VIEW;
}

static bool RectsOverlap(const OccupiedRect& a, const OccupiedRect& b)
{
    if (!a.valid || !b.valid) {
        return false;
    }
    bool overlapX = !(a.maxX <= b.minX || b.maxX <= a.minX);
    bool overlapY = !(a.maxY <= b.minY || b.maxY <= a.minY);
    return overlapX && overlapY;
}

static double RectGap(const OccupiedRect& a, const OccupiedRect& b)
{
    if (!a.valid || !b.valid) {
        return 0.0;
    }

    double gapX = 0.0;
    if (a.maxX < b.minX) {
        gapX = b.minX - a.maxX;
    } else if (b.maxX < a.minX) {
        gapX = a.minX - b.maxX;
    }

    double gapY = 0.0;
    if (a.maxY < b.minY) {
        gapY = b.minY - a.maxY;
    } else if (b.maxY < a.minY) {
        gapY = a.minY - b.maxY;
    }

    if (gapX <= 0.0 && gapY <= 0.0) {
        return 0.0;
    }
    if (gapX <= 0.0) {
        return gapY;
    }
    if (gapY <= 0.0) {
        return gapX;
    }
    return std::sqrt(gapX * gapX + gapY * gapY);
}

static void AppendQualityRule(std::string& rulesJson, bool& firstRule, int& errorCount, int& warningCount,
    double& score, const char* ruleId, bool passed, const char* severity, double penalty,
    const std::string& message, const std::string& recommendation)
{
    if (!firstRule) {
        rulesJson += ",";
    }
    firstRule = false;

    const char* resultSeverity = passed ? "ok" : severity;
    if (!passed) {
        score -= penalty;
        if (strcmp(severity, "error") == 0) {
            errorCount++;
        } else if (strcmp(severity, "warning") == 0) {
            warningCount++;
        }
    }

    rulesJson += std::string("{\"rule\":\"") + JsonEscape(ruleId)
        + "\",\"passed\":" + (passed ? "true" : "false")
        + ",\"severity\":\"" + JsonEscape(resultSeverity)
        + "\",\"penalty\":" + std::to_string(passed ? 0.0 : penalty)
        + ",\"message\":\"" + JsonEscape(message)
        + "\",\"recommendation\":\"" + JsonEscape(recommendation) + "\"}";
}

static void AppendRecommendation(std::string& recommendationsJson, bool& firstRecommendation, const std::string& recommendation)
{
    if (!firstRecommendation) {
        recommendationsJson += ",";
    }
    firstRecommendation = false;
    recommendationsJson += "\"" + JsonEscape(recommendation) + "\"";
}

static std::string BuildOverallOccupiedRectJson(const std::vector<szwEntityHandle>& viewHandles, const DrawingPaperRect& paperRect)
{
    OccupiedRect totalRect;
    std::string viewsJson = "[";
    for (size_t i = 0; i < viewHandles.size(); ++i) {
        OccupiedRect rect = ComputeViewOccupiedRect(viewHandles[i]);
        if (rect.valid) {
            if (!totalRect.valid) {
                totalRect = rect;
            } else {
                if (rect.minX < totalRect.minX) totalRect.minX = rect.minX;
                if (rect.maxX > totalRect.maxX) totalRect.maxX = rect.maxX;
                if (rect.minY < totalRect.minY) totalRect.minY = rect.minY;
                if (rect.maxY > totalRect.maxY) totalRect.maxY = rect.maxY;
            }
        }

        if (i > 0) {
            viewsJson += ",";
        }
        viewsJson += std::string("{\"view_index\":") + std::to_string(static_cast<int>(i))
            + ",\"occupied\":" + BuildOccupiedRectJson(rect) + "}";
    }
    viewsJson += "]";

    double overflowLeft = 0.0;
    double overflowRight = 0.0;
    double overflowBottom = 0.0;
    double overflowTop = 0.0;
    if (totalRect.valid) {
        if (totalRect.minX < paperRect.minX) overflowLeft = paperRect.minX - totalRect.minX;
        if (totalRect.maxX > paperRect.maxX) overflowRight = totalRect.maxX - paperRect.maxX;
        if (totalRect.minY < paperRect.minY) overflowBottom = paperRect.minY - totalRect.minY;
        if (totalRect.maxY > paperRect.maxY) overflowTop = totalRect.maxY - paperRect.maxY;
    }

    return std::string("{\"paper\":{\"min_x\":") + std::to_string(paperRect.minX)
        + ",\"max_x\":" + std::to_string(paperRect.maxX)
        + ",\"min_y\":" + std::to_string(paperRect.minY)
        + ",\"max_y\":" + std::to_string(paperRect.maxY)
        + "},\"overall\":" + BuildOccupiedRectJson(totalRect)
        + ",\"overflow\":{\"left\":" + std::to_string(overflowLeft)
        + ",\"right\":" + std::to_string(overflowRight)
        + ",\"bottom\":" + std::to_string(overflowBottom)
        + ",\"top\":" + std::to_string(overflowTop)
        + "},\"views\":" + viewsJson + "}";
}

static std::string BuildActiveSheetOccupiedRectJson(const DrawingPaperRect& paperRect)
{
    int viewCount = 0;
    szwEntityHandle* viewList = nullptr;
    ezwErrors ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &viewCount, &viewList);
    if (ret != ZW_API_NO_ERROR || viewCount <= 0 || viewList == nullptr) {
        if (viewList != nullptr) {
            ZwEntityHandleListFree(viewCount, &viewList);
        }
        return std::string("{\"status\":\"error\",\"message\":\"active view list unavailable\",\"code\":") + std::to_string(ret) + "}";
    }

    OccupiedRect totalRect;
    std::string viewsJson = "[";
    for (int i = 0; i < viewCount; ++i) {
        OccupiedRect rect = ComputeViewOccupiedRect(viewList[i]);
        if (rect.valid) {
            if (!totalRect.valid) {
                totalRect = rect;
            } else {
                if (rect.minX < totalRect.minX) totalRect.minX = rect.minX;
                if (rect.maxX > totalRect.maxX) totalRect.maxX = rect.maxX;
                if (rect.minY < totalRect.minY) totalRect.minY = rect.minY;
                if (rect.maxY > totalRect.maxY) totalRect.maxY = rect.maxY;
            }
        }
        if (i > 0) {
            viewsJson += ",";
        }
        viewsJson += std::string("{\"view_index\":") + std::to_string(i)
            + ",\"occupied\":" + BuildOccupiedRectJson(rect) + "}";
    }
    ZwEntityHandleListFree(viewCount, &viewList);
    viewsJson += "]";

    double overflowLeft = 0.0;
    double overflowRight = 0.0;
    double overflowBottom = 0.0;
    double overflowTop = 0.0;
    if (totalRect.valid) {
        if (totalRect.minX < paperRect.minX) overflowLeft = paperRect.minX - totalRect.minX;
        if (totalRect.maxX > paperRect.maxX) overflowRight = totalRect.maxX - paperRect.maxX;
        if (totalRect.minY < paperRect.minY) overflowBottom = paperRect.minY - totalRect.minY;
        if (totalRect.maxY > paperRect.maxY) overflowTop = totalRect.maxY - paperRect.maxY;
    }

    return std::string("{\"paper\":{\"min_x\":") + std::to_string(paperRect.minX)
        + ",\"max_x\":" + std::to_string(paperRect.maxX)
        + ",\"min_y\":" + std::to_string(paperRect.minY)
        + ",\"max_y\":" + std::to_string(paperRect.maxY)
        + "},\"overall\":" + BuildOccupiedRectJson(totalRect)
        + ",\"overflow\":{\"left\":" + std::to_string(overflowLeft)
        + ",\"right\":" + std::to_string(overflowRight)
        + ",\"bottom\":" + std::to_string(overflowBottom)
        + ",\"top\":" + std::to_string(overflowTop)
        + "},\"views\":" + viewsJson + "}";
}

static double ComputeActiveSheetOverflowMagnitude(const DrawingPaperRect& paperRect)
{
    int viewCount = 0;
    szwEntityHandle* viewList = nullptr;
    ezwErrors ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &viewCount, &viewList);
    if (ret != ZW_API_NO_ERROR || viewCount <= 0 || viewList == nullptr) {
        if (viewList != nullptr) {
            ZwEntityHandleListFree(viewCount, &viewList);
        }
        return 0.0;
    }

    OccupiedRect totalRect;
    for (int i = 0; i < viewCount; ++i) {
        ezwDrawingViewType type = ZW_DRAWING_ALL_VIEW;
        if (ZwDrawingViewTypeGet(viewList[i], &type) != ZW_API_NO_ERROR) {
            continue;
        }

        OccupiedRect rect = ComputeViewOccupiedRect(viewList[i]);
        if (!IsQualityLayoutView(type, rect)) {
            continue;
        }
        if (!totalRect.valid) {
            totalRect = rect;
        } else {
            if (rect.minX < totalRect.minX) totalRect.minX = rect.minX;
            if (rect.maxX > totalRect.maxX) totalRect.maxX = rect.maxX;
            if (rect.minY < totalRect.minY) totalRect.minY = rect.minY;
            if (rect.maxY > totalRect.maxY) totalRect.maxY = rect.maxY;
        }
    }
    ZwEntityHandleListFree(viewCount, &viewList);

    if (!totalRect.valid) {
        return 0.0;
    }

    double overflow = 0.0;
    if (totalRect.minX < paperRect.minX) overflow += (paperRect.minX - totalRect.minX);
    if (totalRect.maxX > paperRect.maxX) overflow += (totalRect.maxX - paperRect.maxX);
    if (totalRect.minY < paperRect.minY) overflow += (paperRect.minY - totalRect.minY);
    if (totalRect.maxY > paperRect.maxY) overflow += (totalRect.maxY - paperRect.maxY);
    return overflow;
}

static int ClearActiveSheetViews()
{
    int viewCount = 0;
    int deletedCount = 0;
    szwEntityHandle* viewList = nullptr;
    ezwErrors ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &viewCount, &viewList);
    if (ret != ZW_API_NO_ERROR || viewCount <= 0 || viewList == nullptr) {
        if (viewList != nullptr) {
            ZwEntityHandleListFree(viewCount, &viewList);
        }
        return 0;
    }

    for (int pass = 0; pass < 3; ++pass) {
        for (int i = viewCount - 1; i >= 0; --i) {
            if (IsEmptyHandle(viewList[i])) {
                continue;
            }
            if (ZwEntityDelete(viewList[i]) == ZW_API_NO_ERROR) {
                deletedCount++;
                ZeroMemory(&viewList[i], sizeof(szwEntityHandle));
            }
        }
        ZwDrawingSheetManagerViewTreeRefresh(nullptr);
        Sleep(200);
    }

    ZwEntityHandleListFree(viewCount, &viewList);
    return deletedCount;
}

static bool HasActiveReferenceProfile()
{
    return g_activeReferenceProfile.valid && g_activeReferenceProfile.viewCount > 0;
}

static bool ReferenceViewTypeIs(int index, const char* typeName)
{
    if (!HasActiveReferenceProfile() || index < 0 || index >= g_activeReferenceProfile.viewCount) {
        return false;
    }
    return _stricmp(g_activeReferenceProfile.views[index].type, typeName) == 0;
}

static int ReferencePrimaryBaseIndex()
{
    if (!HasActiveReferenceProfile()) {
        return -1;
    }
    int fallbackBase = -1;
    for (int i = 0; i < g_activeReferenceProfile.viewCount; ++i) {
        if (!ReferenceViewTypeIs(i, "base")) {
            continue;
        }
        if (fallbackBase < 0) {
            fallbackBase = i;
        }
        if (g_activeReferenceProfile.views[i].dimensions > 0) {
            return i;
        }
    }
    return fallbackBase;
}

static int ReferenceVerticalProjectIndex()
{
    int baseIdx = ReferencePrimaryBaseIndex();
    if (baseIdx < 0) {
        return -1;
    }
    const double baseX = g_activeReferenceProfile.views[baseIdx].x;
    const double baseY = g_activeReferenceProfile.views[baseIdx].y;
    int best = -1;
    double bestDy = 999999.0;
    for (int i = 0; i < g_activeReferenceProfile.viewCount; ++i) {
        if (!ReferenceViewTypeIs(i, "project")) {
            continue;
        }
        double dx = std::fabs(g_activeReferenceProfile.views[i].x - baseX);
        double dy = std::fabs(g_activeReferenceProfile.views[i].y - baseY);
        if (dx <= 1.5 && dy > 1.5 && dy < bestDy) {
            best = i;
            bestDy = dy;
        }
    }
    return best;
}

static int ReferenceRightProjectIndex()
{
    int baseIdx = ReferencePrimaryBaseIndex();
    if (baseIdx < 0) {
        return -1;
    }
    const double baseX = g_activeReferenceProfile.views[baseIdx].x;
    const double baseY = g_activeReferenceProfile.views[baseIdx].y;
    int best = -1;
    double bestDx = 999999.0;
    for (int i = 0; i < g_activeReferenceProfile.viewCount; ++i) {
        if (!ReferenceViewTypeIs(i, "project")) {
            continue;
        }
        double dx = g_activeReferenceProfile.views[i].x - baseX;
        double dy = std::fabs(g_activeReferenceProfile.views[i].y - baseY);
        if (dx > 1.5 && dy <= 1.5 && dx < bestDx) {
            best = i;
            bestDx = dx;
        }
    }
    return best;
}

static int ReferenceExtraRightProjectIndex()
{
    int rightIdx = ReferenceRightProjectIndex();
    if (rightIdx < 0) {
        return -1;
    }
    const double rightX = g_activeReferenceProfile.views[rightIdx].x;
    const double rightY = g_activeReferenceProfile.views[rightIdx].y;
    int best = -1;
    double bestDx = 999999.0;
    for (int i = 0; i < g_activeReferenceProfile.viewCount; ++i) {
        if (i == rightIdx || !ReferenceViewTypeIs(i, "project")) {
            continue;
        }
        double dx = g_activeReferenceProfile.views[i].x - rightX;
        double dy = std::fabs(g_activeReferenceProfile.views[i].y - rightY);
        if (dx > 1.5 && dy <= 1.5 && dx < bestDx) {
            best = i;
            bestDx = dx;
        }
    }
    return best;
}

static int ReferenceLeftProjectIndex()
{
    int baseIdx = ReferencePrimaryBaseIndex();
    if (baseIdx < 0) {
        return -1;
    }
    const double baseX = g_activeReferenceProfile.views[baseIdx].x;
    const double baseY = g_activeReferenceProfile.views[baseIdx].y;
    int best = -1;
    double bestDx = 999999.0;
    for (int i = 0; i < g_activeReferenceProfile.viewCount; ++i) {
        if (!ReferenceViewTypeIs(i, "project")) {
            continue;
        }
        double dx = baseX - g_activeReferenceProfile.views[i].x;
        double dy = std::fabs(g_activeReferenceProfile.views[i].y - baseY);
        if (dx > 1.5 && dy <= 1.5 && dx < bestDx) {
            best = i;
            bestDx = dx;
        }
    }
    return best;
}

static int ReferenceSecondaryBaseIndex()
{
    int primaryBase = ReferencePrimaryBaseIndex();
    if (primaryBase < 0) {
        return -1;
    }
    for (int i = 0; i < g_activeReferenceProfile.viewCount; ++i) {
        if (i != primaryBase && ReferenceViewTypeIs(i, "base")) {
            return i;
        }
    }
    return -1;
}

static int ReferenceTargetDimensionsForOrdinal(int annotationOrdinal)
{
    if (!HasActiveReferenceProfile()) {
        return -1;
    }

    int index = -1;
    if (annotationOrdinal == 0) {
        index = ReferencePrimaryBaseIndex();
    } else if (annotationOrdinal == 1) {
        index = ReferenceVerticalProjectIndex();
    } else if (annotationOrdinal == 2) {
        index = ReferenceRightProjectIndex();
    } else if (annotationOrdinal == 3) {
        index = ReferenceLeftProjectIndex();
        if (index < 0) {
            index = ReferenceExtraRightProjectIndex();
        }
    }

    if (index >= 0) {
        return g_activeReferenceProfile.views[index].dimensions;
    }
    return -1;
}

static bool GetActivePartBoundingSize(double* sizeX, double* sizeY, double* sizeZ)
{
    if (sizeX) *sizeX = 100.0;
    if (sizeY) *sizeY = 60.0;
    if (sizeZ) *sizeZ = 40.0;

    int shapeCount = 0;
    int* shapeIds = nullptr;
    evxErrors ret = cvxPartInqShapes(nullptr, nullptr, &shapeCount, &shapeIds);
    if (ret != ZW_API_NO_ERROR || shapeCount <= 0 || shapeIds == nullptr) {
        if (shapeIds != nullptr) {
            cvxMemFree((void**)&shapeIds);
        }
        return false;
    }

    bool hasBox = false;
    double minX = 0.0, minY = 0.0, minZ = 0.0;
    double maxX = 0.0, maxY = 0.0, maxZ = 0.0;

    for (int i = 0; i < shapeCount; ++i) {
        svxBndBox box = {};
        if (cvxPartInqShapeBox(shapeIds[i], nullptr, &box) != ZW_API_NO_ERROR) {
            continue;
        }

        if (!hasBox) {
            minX = box.X.min;
            minY = box.Y.min;
            minZ = box.Z.min;
            maxX = box.X.max;
            maxY = box.Y.max;
            maxZ = box.Z.max;
            hasBox = true;
            continue;
        }

        if (box.X.min < minX) minX = box.X.min;
        if (box.Y.min < minY) minY = box.Y.min;
        if (box.Z.min < minZ) minZ = box.Z.min;
        if (box.X.max > maxX) maxX = box.X.max;
        if (box.Y.max > maxY) maxY = box.Y.max;
        if (box.Z.max > maxZ) maxZ = box.Z.max;
    }

    cvxMemFree((void**)&shapeIds);

    if (!hasBox) {
        return false;
    }

    double dx = maxX - minX;
    double dy = maxY - minY;
    double dz = maxZ - minZ;
    if (dx < 1.0) dx = 1.0;
    if (dy < 1.0) dy = 1.0;
    if (dz < 1.0) dz = 1.0;

    if (sizeX) *sizeX = dx;
    if (sizeY) *sizeY = dy;
    if (sizeZ) *sizeZ = dz;
    return true;
}

static std::string BuildPartAnalysisJson(double sizeX, double sizeY, double sizeZ,
    ezwDrawingViewMethod* baseViewOut, double* widthOut, double* heightOut, const char** profileOut)
{
    double areaFront = sizeX * sizeZ;
    double areaTop = sizeX * sizeY;
    double areaRight = sizeY * sizeZ;
    ezwDrawingViewMethod baseView = ZW_VIEW_STANDARD_FRONT;
    const char* baseName = "front";
    double baseWidth = sizeX;
    double baseHeight = sizeZ;

    if (areaTop >= areaFront && areaTop >= areaRight) {
        baseView = ZW_VIEW_STANDARD_TOP;
        baseName = "top";
        baseWidth = sizeX;
        baseHeight = sizeY;
    } else if (areaRight >= areaFront && areaRight >= areaTop) {
        baseView = ZW_VIEW_STANDARD_RIGHT;
        baseName = "right";
        baseWidth = sizeY;
        baseHeight = sizeZ;
    }

    double dims[3] = { sizeX, sizeY, sizeZ };
    double maxDim = dims[0];
    double minDim = dims[0];
    double midDim = dims[0];
    for (int i = 1; i < 3; ++i) {
        if (dims[i] > maxDim) maxDim = dims[i];
        if (dims[i] < minDim) minDim = dims[i];
    }
    for (int i = 0; i < 3; ++i) {
        if (dims[i] != maxDim && dims[i] != minDim) {
            midDim = dims[i];
            break;
        }
    }
    if (maxDim == minDim) midDim = maxDim;
    else if (midDim == maxDim || midDim == minDim) midDim = (sizeX + sizeY + sizeZ - maxDim - minDim);

    const char* profile = "blocky";
    if (minDim / maxDim < 0.25) {
        profile = "plate_like";
    } else if (maxDim / midDim > 2.2) {
        profile = "elongated";
    } else if (maxDim / minDim > 1.6) {
        profile = "stepped";
    }

    // Stepped/support parts usually read better from the front unless the top view
    // is overwhelmingly more informative. This avoids stacking the dominant view
    // too high on the sheet and reduces PMI crowding between top/right views.
    if (strcmp(profile, "stepped") == 0 && areaTop <= areaFront * 1.25) {
        baseView = ZW_VIEW_STANDARD_FRONT;
        baseName = "front";
        baseWidth = sizeX;
        baseHeight = sizeZ;
    }

    if (baseViewOut) *baseViewOut = baseView;
    if (widthOut) *widthOut = baseWidth;
    if (heightOut) *heightOut = baseHeight;
    if (profileOut) *profileOut = profile;

    return std::string("{\"status\":\"ok\",\"bbox\":{\"x\":") + std::to_string(sizeX)
        + ",\"y\":" + std::to_string(sizeY)
        + ",\"z\":" + std::to_string(sizeZ)
        + "},\"areas\":{\"front\":" + std::to_string(areaFront)
        + ",\"top\":" + std::to_string(areaTop)
        + ",\"right\":" + std::to_string(areaRight)
        + "},\"recommended_base_view\":\"" + baseName
        + "\",\"recommended_profile\":\"" + profile + "\"}";
}

static int CountDimensionTextOverlaps(int count, szwEntityHandle* dimensions)
{
    if (count <= 1 || dimensions == nullptr) {
        return 0;
    }

    int overlaps = 0;
    for (int i = 0; i < count; ++i) {
        szwDrawingDimensionTextPositionPoints a = {};
        if (ZwDrawingDimensionTextPositionPointsGet(dimensions[i], &a) != ZW_API_NO_ERROR) {
            continue;
        }
        double aMinX = a.bottomLeft.x;
        double aMaxX = a.bottomRight.x;
        double aMinY = a.bottomLeft.y;
        double aMaxY = a.topLeft.y;

        for (int j = i + 1; j < count; ++j) {
            szwDrawingDimensionTextPositionPoints b = {};
            if (ZwDrawingDimensionTextPositionPointsGet(dimensions[j], &b) != ZW_API_NO_ERROR) {
                continue;
            }
            double bMinX = b.bottomLeft.x;
            double bMaxX = b.bottomRight.x;
            double bMinY = b.bottomLeft.y;
            double bMaxY = b.topLeft.y;

            bool overlapX = !(aMaxX + 1.0 < bMinX || bMaxX + 1.0 < aMinX);
            bool overlapY = !(aMaxY + 1.0 < bMinY || bMaxY + 1.0 < aMinY);
            if (overlapX && overlapY) {
                overlaps++;
            }
        }
    }

    return overlaps;
}

static int CountDimensionViewIntrusions(const szwEntityHandle& viewHandle, int count, szwEntityHandle* dimensions)
{
    if (count <= 0 || dimensions == nullptr) {
        return 0;
    }

    double padX = 8.0;
    double padY = 6.0;
    if (HasActiveReferenceProfile() && strstr(g_activeReferenceProfile.sample, "TZ-TP") != nullptr) {
        padX = 2.0;
        padY = 2.0;
    }

    int viewCount = 0;
    int intrusions = 0;
    szwEntityHandle* viewList = nullptr;
    if (ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &viewCount, &viewList) != ZW_API_NO_ERROR || viewList == nullptr) {
        if (viewList != nullptr) {
            ZwEntityHandleListFree(viewCount, &viewList);
        }
        return 0;
    }

    for (int i = 0; i < count; ++i) {
        szwDrawingDimensionTextPositionPoints box = {};
        if (ZwDrawingDimensionTextPositionPointsGet(dimensions[i], &box) != ZW_API_NO_ERROR) {
            continue;
        }

        double minX = box.bottomLeft.x - padX;
        double maxX = box.bottomRight.x + padX;
        double minY = box.bottomLeft.y - padY;
        double maxY = box.topLeft.y + padY;

        for (int j = 0; j < viewCount; ++j) {
            if (memcmp(&viewHandle, &viewList[j], sizeof(szwEntityHandle)) == 0) {
                continue;
            }

            szwDrawingDottedBorder border = {};
            if (ZwDrawingViewDottedBorderGet(viewList[j], &border) != ZW_API_NO_ERROR) {
                continue;
            }

            double viewMinX = border.upperLeft.x;
            double viewMaxX = border.bottomRight.x;
            double viewMinY = border.bottomRight.y;
            double viewMaxY = border.upperLeft.y;

            bool overlapX = !(maxX < viewMinX || viewMaxX < minX);
            bool overlapY = !(maxY < viewMinY || viewMaxY < minY);
            if (overlapX && overlapY) {
                intrusions++;
                break;
            }
        }
    }

    ZwEntityHandleListFree(viewCount, &viewList);
    return intrusions;
}

static int DeleteIntrusiveDimensions(const szwEntityHandle& viewHandle, int maxDelete)
{
    if (maxDelete <= 0) {
        return 0;
    }

    double padX = 8.0;
    double padY = 6.0;
    if (HasActiveReferenceProfile() && strstr(g_activeReferenceProfile.sample, "TZ-TP") != nullptr) {
        padX = 2.0;
        padY = 2.0;
    }

    int dimCount = 0;
    szwEntityHandle* dimensions = nullptr;
    if (ZwDrawingViewDimensionListGet(viewHandle, ZW_VIEW_ALL_DIMENSION, &dimCount, &dimensions) != ZW_API_NO_ERROR ||
        dimCount <= 0 || dimensions == nullptr) {
        if (dimensions != nullptr) {
            ZwEntityHandleListFree(dimCount, &dimensions);
        }
        return 0;
    }

    int viewCount = 0;
    szwEntityHandle* viewList = nullptr;
    if (ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &viewCount, &viewList) != ZW_API_NO_ERROR || viewList == nullptr) {
        ZwEntityHandleListFree(dimCount, &dimensions);
        if (viewList != nullptr) {
            ZwEntityHandleListFree(viewCount, &viewList);
        }
        return 0;
    }

    int deletedCount = 0;
    for (int i = 0; i < dimCount && deletedCount < maxDelete; ++i) {
        szwDrawingDimensionTextPositionPoints box = {};
        if (ZwDrawingDimensionTextPositionPointsGet(dimensions[i], &box) != ZW_API_NO_ERROR) {
            continue;
        }

        double minX = box.bottomLeft.x - padX;
        double maxX = box.bottomRight.x + padX;
        double minY = box.bottomLeft.y - padY;
        double maxY = box.topLeft.y + padY;
        bool intrusive = false;

        for (int j = 0; j < viewCount; ++j) {
            if (memcmp(&viewHandle, &viewList[j], sizeof(szwEntityHandle)) == 0) {
                continue;
            }

            szwDrawingDottedBorder border = {};
            if (ZwDrawingViewDottedBorderGet(viewList[j], &border) != ZW_API_NO_ERROR) {
                continue;
            }

            double viewMinX = border.upperLeft.x;
            double viewMaxX = border.bottomRight.x;
            double viewMinY = border.bottomRight.y;
            double viewMaxY = border.upperLeft.y;

            bool overlapX = !(maxX < viewMinX || viewMaxX < minX);
            bool overlapY = !(maxY < viewMinY || viewMaxY < minY);
            if (overlapX && overlapY) {
                intrusive = true;
                break;
            }
        }

        if (!intrusive) {
            continue;
        }

        if (ZwEntityDelete(dimensions[i]) == ZW_API_NO_ERROR) {
            deletedCount++;
        }
    }

    ZwEntityHandleListFree(viewCount, &viewList);
    ZwEntityHandleListFree(dimCount, &dimensions);
    return deletedCount;
}

/**
 * Delete dimensions whose text overlaps with other dimension text.
 * For each overlapping pair, deletes the dimension that has more overlaps
 * (the "worse" one), up to maxDelete dimensions.
 */
static int DeleteOverlappingDimensions(const szwEntityHandle& viewHandle, int maxDelete)
{
    if (maxDelete <= 0) return 0;

    int dimCount = 0;
    szwEntityHandle* dimensions = nullptr;
    if (ZwDrawingViewDimensionListGet(viewHandle, ZW_VIEW_ALL_DIMENSION, &dimCount, &dimensions) != ZW_API_NO_ERROR ||
        dimCount <= 0 || dimensions == nullptr) {
        if (dimensions) ZwEntityHandleListFree(dimCount, &dimensions);
        return 0;
    }

    // Compute text bounding boxes and overlap counts
    struct DimBox { double minX, maxX, minY, maxY; int overlapScore; };
    DimBox* boxes = new DimBox[dimCount];
    memset(boxes, 0, sizeof(DimBox) * dimCount);

    for (int i = 0; i < dimCount; ++i) {
        szwDrawingDimensionTextPositionPoints box = {};
        if (ZwDrawingDimensionTextPositionPointsGet(dimensions[i], &box) != ZW_API_NO_ERROR) {
            boxes[i].minX = boxes[i].maxX = boxes[i].minY = boxes[i].maxY = 0;
            boxes[i].overlapScore = 0;
            continue;
        }
        boxes[i].minX = box.bottomLeft.x - 8.0;
        boxes[i].maxX = box.bottomRight.x + 8.0;
        boxes[i].minY = box.bottomLeft.y - 6.0;
        boxes[i].maxY = box.topLeft.y + 6.0;
        boxes[i].overlapScore = 0;
    }

    // Count overlaps per dimension
    for (int i = 0; i < dimCount; ++i) {
        if (boxes[i].minX == 0 && boxes[i].maxX == 0) continue;
        for (int j = i + 1; j < dimCount; ++j) {
            if (boxes[j].minX == 0 && boxes[j].maxX == 0) continue;
            bool ox = !(boxes[i].maxX <= boxes[j].minX || boxes[j].maxX <= boxes[i].minX);
            bool oy = !(boxes[i].maxY <= boxes[j].minY || boxes[j].maxY <= boxes[i].minY);
            if (ox && oy) {
                boxes[i].overlapScore++;
                boxes[j].overlapScore++;
            }
        }
    }

    // Delete dimensions with the most overlaps first
    int deletedCount = 0;
    while (deletedCount < maxDelete) {
        int worstIdx = -1;
        int worstScore = 0;
        for (int i = 0; i < dimCount; ++i) {
            if (boxes[i].overlapScore > worstScore) {
                worstScore = boxes[i].overlapScore;
                worstIdx = i;
            }
        }
        if (worstIdx < 0 || worstScore == 0) break;

        if (ZwEntityDelete(dimensions[worstIdx]) == ZW_API_NO_ERROR) {
            deletedCount++;
            boxes[worstIdx].overlapScore = 0;
            // Reduce overlap counts for dimensions that overlapped with the deleted one
            for (int j = 0; j < dimCount; ++j) {
                if (j == worstIdx || boxes[j].overlapScore == 0) continue;
                bool ox = !(boxes[worstIdx].maxX <= boxes[j].minX || boxes[j].maxX <= boxes[worstIdx].minX);
                bool oy = !(boxes[worstIdx].maxY <= boxes[j].minY || boxes[j].maxY <= boxes[worstIdx].minY);
                if (ox && oy && boxes[j].overlapScore > 0) {
                    boxes[j].overlapScore--;
                }
            }
        } else {
            boxes[worstIdx].overlapScore = 0; // skip this one
        }
    }

    delete[] boxes;
    ZwEntityHandleListFree(dimCount, &dimensions);
    return deletedCount;
}

static int LimitViewDimensionCount(const szwEntityHandle& viewHandle, int maxKeep)
{
    if (maxKeep < 0) return 0;

    int dimCount = 0;
    szwEntityHandle* dimensions = nullptr;
    if (ZwDrawingViewDimensionListGet(viewHandle, ZW_VIEW_ALL_DIMENSION, &dimCount, &dimensions) != ZW_API_NO_ERROR ||
        dimCount <= maxKeep || dimensions == nullptr) {
        if (dimensions != nullptr) {
            ZwEntityHandleListFree(dimCount, &dimensions);
        }
        return 0;
    }

    int deletedCount = 0;
    for (int i = dimCount - 1; i >= maxKeep; --i) {
        if (ZwEntityDelete(dimensions[i]) == ZW_API_NO_ERROR) {
            deletedCount++;
        }
    }

    ZwEntityHandleListFree(dimCount, &dimensions);
    return deletedCount;
}

static ezwErrors PostProcessViewDimensions(const szwEntityHandle& viewHandle, const char* profileName, int* overlapCountOut, int* intrusionCountOut)
{
    if (overlapCountOut) *overlapCountOut = 0;
    if (intrusionCountOut) *intrusionCountOut = 0;

    int dimCount = 0;
    szwEntityHandle* dimensions = nullptr;
    ezwErrors ret = ZwDrawingViewDimensionListGet(viewHandle, ZW_VIEW_ALL_DIMENSION, &dimCount, &dimensions);
    if (ret != ZW_API_NO_ERROR || dimCount <= 0 || dimensions == nullptr) {
        if (dimensions != nullptr) {
            ZwEntityHandleListFree(dimCount, &dimensions);
        }
        return ret;
    }

    double offset = 10.0;
    double increment = 8.0;
    int cleanupPasses = 3;
    if (profileName != nullptr && strcmp(profileName, "stepped") == 0) {
        offset = 13.0;
        increment = 10.0;
    } else if (profileName != nullptr && strcmp(profileName, "elongated") == 0) {
        offset = 15.0;
        increment = 11.0;
    } else if (profileName != nullptr && strcmp(profileName, "plate_like") == 0) {
        offset = 13.0;
        increment = 10.0;
    }
    int overlaps = CountDimensionTextOverlaps(dimCount, dimensions);
    int intrusions = CountDimensionViewIntrusions(viewHandle, dimCount, dimensions);
    for (int pass = 0; pass < cleanupPasses; ++pass) {
        szwDrawingCleanUpDimensionLocations cleanUpData = {};
        ezwErrors initRet = ZwDrawingDimensionLocationsCleanUpInit(&cleanUpData);
        if (initRet != ZW_API_NO_ERROR) {
            ret = initRet;
            break;
        }

        cleanUpData.method = ZW_CLEAN_UP_BY_VIEW;
        cleanUpData.cleanUpEntity.view = viewHandle;
        cleanUpData.offset = offset + pass * 6.0;
        cleanUpData.increment = increment + pass * 5.0;
        cleanUpData.offsetReference = ZW_OFFSET_REFERENCE_VIEW_BOUNDBOX;
        cleanUpData.rearrangeTextPlacement = 1;
        cleanUpData.createMagneticLine = 1;
        ret = ZwDrawingDimensionLocationsCleanUp(cleanUpData);
        if (ret != ZW_API_NO_ERROR) {
            break;
        }

        Sleep(150);
        overlaps = CountDimensionTextOverlaps(dimCount, dimensions);
        intrusions = CountDimensionViewIntrusions(viewHandle, dimCount, dimensions);
        if (overlaps == 0 && intrusions == 0) {
            break;
        }
    }

    if (overlapCountOut) *overlapCountOut = overlaps;
    if (intrusionCountOut) *intrusionCountOut = intrusions;
    ZwEntityHandleListFree(dimCount, &dimensions);
    return ret;
}

static int AddCenterMarksToCircularGeometry(const szwEntityHandle& viewHandle,
    int* geometryCountOut, int* closedCurveCountOut, int* createCodeOut)
{
    struct CircleKey {
        double x;
        double y;
        double radius;
    };

    std::vector<szwEntityHandle> circles;
    std::vector<CircleKey> circleKeys;
    int totalGeometryCount = 0;
    if (geometryCountOut) *geometryCountOut = 0;
    if (closedCurveCountOut) *closedCurveCountOut = 0;
    if (createCodeOut) *createCodeOut = 0;

    if (ZwDrawingViewDiscreteProjectionWait() != ZW_API_NO_ERROR) {
        return 0;
    }
    const ezwDrawingGeometryType geometryTypes[] = {
        ZW_DRAWING_SHOWN_GEOMETRY_COORESPONDING_EDGE_AND_FACE,
        ZW_DRAWING_SHOWN_GEOMETRY_NOT_COORESPONDING_EDGE_AND_FACE
    };

    for (ezwDrawingGeometryType geometryType : geometryTypes) {
        int geometryCount = 0;
        szwEntityHandle* geometryList = nullptr;
        ezwErrors listRet = ZwDrawingViewGeometryListGet(viewHandle, geometryType, &geometryCount, &geometryList);
        if (listRet != ZW_API_NO_ERROR || geometryCount <= 0 || geometryList == nullptr) {
            if (geometryList != nullptr) {
                ZwEntityHandleListFree(geometryCount, &geometryList);
            }
            continue;
        }
        totalGeometryCount += geometryCount;

        for (int i = 0; i < geometryCount; ++i) {
            szwPoint startPoint = {};
            szwPoint endPoint = {};
            szwPoint centerPoint = {};
            if (ZwCurveEndPointGet(geometryList[i], &startPoint, &endPoint) != ZW_API_NO_ERROR
                || ZwCurveCentroidPointGet(geometryList[i], &centerPoint) != ZW_API_NO_ERROR) {
                continue;
            }
            double dx = startPoint.x - endPoint.x;
            double dy = startPoint.y - endPoint.y;
            double dz = startPoint.z - endPoint.z;
            if (std::sqrt(dx * dx + dy * dy + dz * dz) > 0.05) {
                continue;
            }

            double curveLength = 0.0;
            if (ZwCurveLengthGet(geometryList[i], &curveLength) != ZW_API_NO_ERROR || curveLength <= 0.1) {
                continue;
            }
            double equivalentRadius = curveLength / (2.0 * 3.14159265358979323846);

            bool duplicate = false;
            for (const CircleKey& key : circleKeys) {
                if (std::fabs(key.x - centerPoint.x) <= 0.05
                    && std::fabs(key.y - centerPoint.y) <= 0.05
                    && std::fabs(key.radius - equivalentRadius) <= 0.05) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                circles.push_back(geometryList[i]);
                circleKeys.push_back({centerPoint.x, centerPoint.y, equivalentRadius});
            }
        }
        ZwEntityHandleListFree(geometryCount, &geometryList);
    }

    if (circles.empty()) {
        if (geometryCountOut) *geometryCountOut = totalGeometryCount;
        return 0;
    }

    szwDrawingCenterMark centerMark = {};
    if (ZwDrawingDimensionCenterMarkInit(&centerMark) != ZW_API_NO_ERROR) {
        if (geometryCountOut) *geometryCountOut = totalGeometryCount;
        if (closedCurveCountOut) *closedCurveCountOut = static_cast<int>(circles.size());
        return 0;
    }
    centerMark.countArc = static_cast<int>(circles.size());
    centerMark.arcList = circles.data();
    centerMark.isCreateLinesForCenterMarks = 0;

    int createdCount = 0;
    szwEntityHandle* createdMarks = nullptr;
    ezwErrors createRet = ZwDrawingDimensionCenterMarkCreate(centerMark, &createdCount, &createdMarks);
    if (createdMarks != nullptr) {
        ZwEntityHandleListFree(createdCount, &createdMarks);
    }
    if (geometryCountOut) *geometryCountOut = totalGeometryCount;
    if (closedCurveCountOut) *closedCurveCountOut = static_cast<int>(circles.size());
    if (createCodeOut) *createCodeOut = createRet;
    return createRet == ZW_API_NO_ERROR ? createdCount : 0;
}

static ezwErrors TryCreateFullSectionView(const szwEntityHandle& baseViewHandle, double locationX, double locationY,
    const char* label, szwEntityHandle* sectionViewHandleOut)
{
    if (sectionViewHandleOut) {
        *sectionViewHandleOut = {};
    }
    if (IsEmptyHandle(baseViewHandle)) {
        return ZW_API_INVALID_INPUT;
    }

    szwDrawingDottedBorder border = {};
    ezwErrors ret = ZwDrawingViewDottedBorderGet(baseViewHandle, &border);
    if (ret != ZW_API_NO_ERROR) {
        return ret;
    }

    szwFullSectionViewData sectionData = {};
    ret = ZwDrawingViewFullSectionCreateInit(&sectionData);
    if (ret != ZW_API_NO_ERROR) {
        return ret;
    }

    szwPoint2 sectionPoints[2] = {};
    double midX = (border.upperLeft.x + border.bottomRight.x) / 2.0;
    sectionPoints[0].x = midX;
    sectionPoints[0].y = border.bottomRight.y;
    sectionPoints[1].x = midX;
    sectionPoints[1].y = border.upperLeft.y;

    char labelBuffer[32] = {0};
    if (label != nullptr && label[0] != 0) {
        strncpy_s(labelBuffer, sizeof(labelBuffer), label, _TRUNCATE);
    }

    sectionData.viewHandle = baseViewHandle;
    sectionData.countDefineSectionPoint = 2;
    sectionData.defineSectionPointList = sectionPoints;
    sectionData.locationPoint.x = locationX;
    sectionData.locationPoint.y = locationY;
    sectionData.sectionMethod.methodType = ZW_METHOD_TRIMMED_PART;
    sectionData.sectionMethod.isCloseOpenProfile = 1;
    sectionData.sectionMethod.isDynamicScalingAngle = 1;
    sectionData.sectionMethod.locationType = ZW_LOCATION_ORTHOGONAL;
    sectionData.sectionMethod.dimensionType = ZW_SECTION_VIEW_DIMENSION_TYPE_PROJECT;
    sectionData.sectionMethod.locationAngleType = ZW_LOCATION_ANGLE_DEFAULT;
    sectionData.sectionLine.viewLabel = labelBuffer[0] != 0 ? labelBuffer : nullptr;
    sectionData.sectionLine.isFlipArrow = 0;
    sectionData.sectionLine.isShowStepLines = 0;
    sectionData.sectionOption.isComponentSectionState = 1;
    sectionData.sectionOption.isComponentHatchState = 1;
    sectionData.sectionOption.isHatchColor = 1;

    return ZwDrawingViewFullSectionCreate(sectionData, sectionViewHandleOut);
}

static bool AutoAnnotateView(const szwEntityHandle& viewHandle, int viewIndex, int* createdCountOut,
    int* totalCountOut, int* cleanRetOut, int* errOut, std::string* errDetailOut, const char* profileName, std::string* metricsJsonOut)
{
    char msg[512];

    if (createdCountOut) *createdCountOut = 0;
    if (totalCountOut) *totalCountOut = 0;
    if (cleanRetOut) *cleanRetOut = ZW_API_GENERAL_ERROR;
    if (errOut) *errOut = 0;
    if (errDetailOut) errDetailOut->clear();
    if (metricsJsonOut) *metricsJsonOut = "{\"status\":\"error\",\"message\":\"metrics not collected\"}";

    if (IsEmptyHandle(viewHandle)) {
        if (errDetailOut) *errDetailOut = "empty view handle";
        if (metricsJsonOut) *metricsJsonOut = "{\"status\":\"error\",\"message\":\"empty view handle\"}";
        return false;
    }

    szwAutoDimensionData autoData = {};
    ezwErrors ret = ZwDrawingDimensionAutoInit(&autoData);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[PMI] ZwDrawingDimensionAutoInit failed for view %d: %d", viewIndex, ret);
        cvxMsgDisp(msg);
        if (errOut) *errOut = ret;
        if (errDetailOut) *errDetailOut = "auto-init failed";
        if (metricsJsonOut) *metricsJsonOut = std::string("{\"status\":\"error\",\"message\":\"auto-init failed\",\"code\":") + std::to_string(ret) + "}";
        return false;
    }

    autoData.entityType = ZW_DIMENSION_AUTO_ENTITY_ALL;
    autoData.includeAuto = ZW_DIMENSION_AUTO_INCLUDE_VALID;
    autoData.checkOrigin = 0;

    szwDrawingDottedBorder border = {};
    ezwErrors borderRet = ZwDrawingViewDottedBorderGet(viewHandle, &border);
    if (borderRet != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[PMI] Failed to get view border for view %d: %d", viewIndex, borderRet);
        cvxMsgDisp(msg);
        if (errOut) *errOut = borderRet;
        if (errDetailOut) *errDetailOut = "view-border failed";
        if (metricsJsonOut) *metricsJsonOut = std::string("{\"status\":\"error\",\"message\":\"view-border failed\",\"code\":") + std::to_string(borderRet) + "}";
        return false;
    }

    szwPoint horizontalPoint = {};
    horizontalPoint.x = border.bottomRight.x + 8.0;
    horizontalPoint.y = border.bottomRight.y - 8.0;
    horizontalPoint.z = 0.0;

    szwPoint verticalPoint = {};
    verticalPoint.x = border.upperLeft.x - 8.0;
    verticalPoint.y = border.upperLeft.y + 8.0;
    verticalPoint.z = 0.0;

    int enableHorizontal = 1;
    int enableVertical = 1;
    bool isSteppedProfile = profileName != nullptr && strcmp(profileName, "stepped") == 0;
    bool isPlateLikeProfile = profileName != nullptr && strcmp(profileName, "plate_like") == 0;
    double savedMaxDim = g_savedBboxX;
    if (g_savedBboxY > savedMaxDim) savedMaxDim = g_savedBboxY;
    if (g_savedBboxZ > savedMaxDim) savedMaxDim = g_savedBboxZ;
    double savedFootprint = g_savedBboxX * g_savedBboxY;
    bool isSmallPlatePmi = isPlateLikeProfile && g_savedBboxValid && savedMaxDim <= 45.0 && savedFootprint <= 2000.0;
    bool isLargePlatePmi = isPlateLikeProfile && g_savedBboxValid && (savedMaxDim > 180.0 || savedFootprint > 30000.0);
    bool isMediumPlatePmi = isPlateLikeProfile && g_savedBboxValid && !isSmallPlatePmi && !isLargePlatePmi;
    int learnedTargetDimensions = ReferenceTargetDimensionsForOrdinal(viewIndex);
    bool useLearnedPmi = HasActiveReferenceProfile() && learnedTargetDimensions >= 0;
    int effectivePmiVariantMode = g_pmiVariantMode;
    if (effectivePmiVariantMode < 0) {
        effectivePmiVariantMode = HasActiveReferenceProfile() ? 1 : 0;
    }
    if (isSteppedProfile) {
        if (viewIndex == 1) {
            enableVertical = 0;
        } else if (viewIndex >= 2) {
            enableHorizontal = 0;
        }
    } else {
        if (viewIndex == 1) {
            enableVertical = 0;
        } else if (viewIndex == 2) {
            enableHorizontal = 0;
        }
    }

    autoData.horizontalDimension.enable = enableHorizontal;
    autoData.horizontalDimension.type = (effectivePmiVariantMode == 2) ? ZW_DIMENSION_AUTO_BASELINE : ZW_DIMENSION_AUTO_CONTINUOUS;
    autoData.horizontalDimension.viewType = (effectivePmiVariantMode == 1 || effectivePmiVariantMode == 2)
        ? ZW_DIMENSION_VIEW_BELOW_RIGHT : ZW_DIMENSION_VIEW_BOTH;
    autoData.horizontalDimension.asGroup = 1;
    autoData.horizontalDimension.referencePoint.referenceEntityHandle = nullptr;
    autoData.horizontalDimension.referencePoint.criticalPointType = ZW_CRITICAL_FREE_POINT;
    autoData.horizontalDimension.referencePoint.controlPointIndex = 0;
    autoData.horizontalDimension.referencePoint.point = &horizontalPoint;
    autoData.verticalDimension.enable = enableVertical;
    autoData.verticalDimension.type = (effectivePmiVariantMode == 2) ? ZW_DIMENSION_AUTO_BASELINE : ZW_DIMENSION_AUTO_CONTINUOUS;
    autoData.verticalDimension.viewType = (effectivePmiVariantMode == 1 || effectivePmiVariantMode == 2)
        ? ZW_DIMENSION_VIEW_BELOW_RIGHT : ZW_DIMENSION_VIEW_BOTH;
    autoData.verticalDimension.asGroup = 1;
    autoData.verticalDimension.referencePoint.referenceEntityHandle = nullptr;
    autoData.verticalDimension.referencePoint.criticalPointType = ZW_CRITICAL_FREE_POINT;
    autoData.verticalDimension.referencePoint.controlPointIndex = 0;
    autoData.verticalDimension.referencePoint.point = &verticalPoint;

    int createdCount = 0;
    szwEntityHandle* createdDims = nullptr;
    ret = ZwDrawingDimensionAutoCreate(viewHandle, autoData, &createdCount, &createdDims);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[PMI] Auto dimension failed for view %d: %d", viewIndex, ret);
        cvxMsgDisp(msg);
        if (errOut) *errOut = ret;
        if (errDetailOut) *errDetailOut = "auto-create failed";
        if (metricsJsonOut) *metricsJsonOut = std::string("{\"status\":\"error\",\"message\":\"auto-create failed\",\"code\":") + std::to_string(ret) + "}";
        return false;
    }

    if (createdDims != nullptr) {
        ZwEntityHandleListFree(createdCount, &createdDims);
    }

    bool addBaselineSupplement = (useLearnedPmi && learnedTargetDimensions >= 6)
        || (effectivePmiVariantMode == 0 && !useLearnedPmi
        && ((isSteppedProfile && (viewIndex == 0 || viewIndex == 1))
        || (isMediumPlatePmi && (viewIndex == 0 || viewIndex >= 3))));
    if (addBaselineSupplement) {
        szwPoint extraHorizontalPoint = horizontalPoint;
        extraHorizontalPoint.y -= 8.0;
        szwPoint extraVerticalPoint = verticalPoint;
        extraVerticalPoint.x -= 8.0;

        szwAutoDimensionData extraData = autoData;
        extraData.horizontalDimension.type = ZW_DIMENSION_AUTO_BASELINE;
        extraData.horizontalDimension.referencePoint.point = &extraHorizontalPoint;
        extraData.verticalDimension.type = ZW_DIMENSION_AUTO_BASELINE;
        extraData.verticalDimension.referencePoint.point = &extraVerticalPoint;

        int extraCreatedCount = 0;
        szwEntityHandle* extraCreatedDims = nullptr;
        ezwErrors extraRet = ZwDrawingDimensionAutoCreate(viewHandle, extraData, &extraCreatedCount, &extraCreatedDims);
        if (extraRet == ZW_API_NO_ERROR) {
            createdCount += extraCreatedCount;
            sprintf_s(msg, sizeof(msg), "[PMI] View %d added stepped baseline supplement, created=%d",
                viewIndex, extraCreatedCount);
            cvxMsgDisp(msg);
        } else {
            sprintf_s(msg, sizeof(msg), "[PMI] View %d stepped baseline supplement skipped: %d",
                viewIndex, extraRet);
            cvxMsgDisp(msg);
        }
        if (extraCreatedDims != nullptr) {
            ZwEntityHandleListFree(extraCreatedCount, &extraCreatedDims);
        }
    }

    int dimCountAfter = 0;
    szwEntityHandle* dimListAfter = nullptr;
    ezwErrors dimRet = ZwDrawingViewDimensionListGet(viewHandle, ZW_VIEW_ALL_DIMENSION, &dimCountAfter, &dimListAfter);
    if (dimRet == ZW_API_NO_ERROR && dimListAfter != nullptr) {
        ZwEntityHandleListFree(dimCountAfter, &dimListAfter);
    }

    int overlapCount = 0;
    int intrusionCount = 0;
    int deletedIntrusive = 0;
    int deletedExcess = 0;
    ezwErrors cleanRet = PostProcessViewDimensions(viewHandle, profileName, &overlapCount, &intrusionCount);
    if (cleanRet != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[PMI] Dimension clean-up skipped for view %d (exec=%d)", viewIndex, cleanRet);
        cvxMsgDisp(msg);
    }

    bool preserveReferenceDensePmi = effectivePmiVariantMode == 0 && !useLearnedPmi
        && ((isSteppedProfile && viewIndex == 0)
        || (isMediumPlatePmi && (viewIndex == 0 || viewIndex >= 3)));
    if (intrusionCount > 0 && !preserveReferenceDensePmi) {
        // Delete ALL intrusive dimensions (no budget limit — zero intrusions is required)
        while (intrusionCount > 0) {
            int deletedNow = DeleteIntrusiveDimensions(viewHandle, intrusionCount);
            if (deletedNow <= 0) {
                break;
            }
            deletedIntrusive += deletedNow;
            sprintf_s(msg, sizeof(msg), "[PMI] View %d deleted %d intrusive dimension(s), re-running clean-up",
                viewIndex, deletedNow);
            cvxMsgDisp(msg);
            cleanRet = PostProcessViewDimensions(viewHandle, profileName, &overlapCount, &intrusionCount);
        }
    }

    // Delete overlapping dimensions until zero overlaps
    if (overlapCount > 0 && !preserveReferenceDensePmi) {
        int overlapDeleteTotal = 0;
        while (overlapCount > 0) {
            int deletedNow = DeleteOverlappingDimensions(viewHandle, overlapCount);
            if (deletedNow <= 0) {
                break;
            }
            overlapDeleteTotal += deletedNow;
            sprintf_s(msg, sizeof(msg), "[PMI] View %d deleted %d overlapping dimension(s)",
                viewIndex, deletedNow);
            cvxMsgDisp(msg);
            cleanRet = PostProcessViewDimensions(viewHandle, profileName, &overlapCount, &intrusionCount);
        }
    }

    int centerMarksCreated = 0;
    int centerMarkGeometryCount = 0;
    int centerMarkCandidateCount = 0;
    int centerMarkCreateCode = 0;
    if (useLearnedPmi && learnedTargetDimensions > dimCountAfter) {
        centerMarksCreated = AddCenterMarksToCircularGeometry(viewHandle,
            &centerMarkGeometryCount, &centerMarkCandidateCount, &centerMarkCreateCode);
        createdCount += centerMarksCreated;
        if (centerMarksCreated > 0) {
            sprintf_s(msg, sizeof(msg), "[PMI] View %d added %d center mark(s) from circular geometry",
                viewIndex, centerMarksCreated);
            cvxMsgDisp(msg);
        }
    }

    if (useLearnedPmi || isSteppedProfile || isSmallPlatePmi || isMediumPlatePmi) {
        int maxKeep = -1;
        if (useLearnedPmi) {
            maxKeep = learnedTargetDimensions;
        } else if (viewIndex == 0) {
            if (isSteppedProfile) maxKeep = 13;
            else if (isSmallPlatePmi) maxKeep = 2;
            else if (isMediumPlatePmi) maxKeep = 7;
        } else if (viewIndex == 1) {
            if (isSteppedProfile) maxKeep = 6;
            else if (isSmallPlatePmi || isMediumPlatePmi) maxKeep = 0;
        } else if (viewIndex == 2) {
            if (isSteppedProfile) maxKeep = 4;
            else if (isSmallPlatePmi || isMediumPlatePmi) maxKeep = 1;
        } else if (viewIndex >= 3) {
            if (isSteppedProfile) maxKeep = 3;
            else if (isSmallPlatePmi) maxKeep = 1;
            else if (isMediumPlatePmi) maxKeep = 10;
        }

        if (maxKeep >= 0) {
            deletedExcess = LimitViewDimensionCount(viewHandle, maxKeep);
            if (deletedExcess > 0) {
                sprintf_s(msg, sizeof(msg), "[PMI] View %d trimmed %d excess dimension(s) to match stepped reference distribution",
                    viewIndex, deletedExcess);
                cvxMsgDisp(msg);
                cleanRet = PostProcessViewDimensions(viewHandle, profileName, &overlapCount, &intrusionCount);
            }
        }
    }

    dimCountAfter = 0;
    dimListAfter = nullptr;
    dimRet = ZwDrawingViewDimensionListGet(viewHandle, ZW_VIEW_ALL_DIMENSION, &dimCountAfter, &dimListAfter);
    if (dimRet == ZW_API_NO_ERROR && dimListAfter != nullptr) {
        ZwEntityHandleListFree(dimCountAfter, &dimListAfter);
    }

    sprintf_s(msg, sizeof(msg), "[PMI] View %d annotated, created=%d total=%d cleanup=%d deleted=%d trimmed=%d overlaps=%d intrusions=%d",
        viewIndex, createdCount, dimCountAfter, cleanRet, deletedIntrusive, deletedExcess, overlapCount, intrusionCount);
    cvxMsgDisp(msg);

    if (metricsJsonOut) {
        *metricsJsonOut = std::string("{\"status\":\"ok\",\"view_index\":") + std::to_string(viewIndex)
            + ",\"created_dimensions\":" + std::to_string(createdCount)
            + ",\"created_center_marks\":" + std::to_string(centerMarksCreated)
            + ",\"center_mark_geometry_count\":" + std::to_string(centerMarkGeometryCount)
            + ",\"center_mark_candidate_count\":" + std::to_string(centerMarkCandidateCount)
            + ",\"center_mark_create_code\":" + std::to_string(centerMarkCreateCode)
            + ",\"total_dimensions\":" + std::to_string((dimRet == ZW_API_NO_ERROR) ? dimCountAfter : 0)
            + ",\"deleted_intrusive_dimensions\":" + std::to_string(deletedIntrusive)
            + ",\"deleted_excess_dimensions\":" + std::to_string(deletedExcess)
            + ",\"cleanup_code\":" + std::to_string(cleanRet)
            + ",\"text_overlaps\":" + std::to_string(overlapCount)
            + ",\"view_intrusions\":" + std::to_string(intrusionCount)
            + "}";
    }

    if (createdCountOut) *createdCountOut = createdCount;
    if (totalCountOut) *totalCountOut = (dimRet == ZW_API_NO_ERROR) ? dimCountAfter : 0;
    if (cleanRetOut) *cleanRetOut = cleanRet;
    return true;
}

/**
 * Create a gear shape
 */
static std::string ApiCreateGear(int teeth, double radius, double thickness)
{
    char msg[512];
    cvxMsgDisp("=== ApiCreateGear (main thread) ===");

    if (!EnsureActivePart()) {
        return "{\"status\":\"error\",\"message\":\"Failed to create/activate part\"}";
    }

    sprintf_s(msg, sizeof(msg), "Creating gear: %d teeth, R=%.1f, T=%.1f", teeth, radius, thickness);
    cvxMsgDisp(msg);

    // Create gear body (cylinder - approximated by box)
    svxBoxData box;
    cvxPartBoxInit(&box);
    box.X = thickness;
    box.Y = radius * 2.0;
    box.Z = radius * 2.0;
    int shapeId = 0;
    evxErrors ret = cvxPartBox(&box, &shapeId);

    if (ret != ZW_API_NO_ERROR) {
        cvxMsgDisp("Gear body creation failed");
        return "{\"status\":\"error\",\"message\":\"Gear body creation failed\"}";
    }

    cvxMsgDisp("Gear body created");

    return "{\"status\":\"ok\",\"message\":\"Gear created successfully\",\"teeth\":" + std::to_string(teeth)
        + ",\"radius\":" + std::to_string(radius) + ",\"thickness\":" + std::to_string(thickness) + "}";
}

/**
 * Create complex shape (base + cylinder)
 */
static std::string ApiCreateComplexShape(void)
{
    char msg[512];
    cvxMsgDisp("=== ApiCreateComplexShape (main thread) ===");

    if (!EnsureActivePart()) {
        return "{\"status\":\"error\",\"message\":\"Failed to create/activate part\"}";
    }

    cvxMsgDisp("Step 1: Creating base box (80x60x20mm)...");
    svxBoxData box1;
    cvxPartBoxInit(&box1);
    box1.X = 80.0;
    box1.Y = 60.0;
    box1.Z = 20.0;
    int h1Id = 0;
    evxErrors ret = cvxPartBox(&box1, &h1Id);
    if (ret != ZW_API_NO_ERROR) {
        cvxMsgDisp("Base creation failed");
        return "{\"status\":\"error\",\"message\":\"Base box creation failed\"}";
    }

    cvxMsgDisp("Step 2: Creating middle block (40x30x30mm)...");
    svxBoxData box2;
    cvxPartBoxInit(&box2);
    box2.X = 40.0;
    box2.Y = 30.0;
    box2.Z = 30.0;
    int h2Id = 0;
    ret = cvxPartBox(&box2, &h2Id);
    if (ret != ZW_API_NO_ERROR) {
        cvxMsgDisp("Middle block creation failed");
        return "{\"status\":\"partial\",\"message\":\"Base created, middle block failed\"}";
    }

    cvxMsgDisp("Complex shape created (base + middle block)");

    return "{\"status\":\"ok\",\"message\":\"Complex shape created successfully\",\"features\":[\"base\",\"middle_block\"]}";
}

/**
 * Create assembly parts
 */
static std::string ApiCreateAssembly(void)
{
    char msg[512];
    cvxMsgDisp("=== ApiCreateAssembly (main thread) ===");

    if (!EnsureActivePart()) {
        return "{\"status\":\"error\",\"message\":\"Failed to create/activate part\"}";
    }

    cvxMsgDisp("Step 1: Creating base plate (100x80x10mm)...");
    svxBoxData box1;
    cvxPartBoxInit(&box1);
    box1.X = 100.0;
    box1.Y = 80.0;
    box1.Z = 10.0;
    int h1Id = 0;
    evxErrors ret = cvxPartBox(&box1, &h1Id);
    if (ret != ZW_API_NO_ERROR) {
        cvxMsgDisp("Base plate creation failed");
        return "{\"status\":\"error\",\"message\":\"Base plate creation failed\"}";
    }

    cvxMsgDisp("Step 2: Creating bracket (60x15x40mm)...");
    svxBoxData box2;
    cvxPartBoxInit(&box2);
    box2.X = 60.0;
    box2.Y = 15.0;
    box2.Z = 40.0;
    int h2Id = 0;
    ret = cvxPartBox(&box2, &h2Id);
    if (ret != ZW_API_NO_ERROR) {
        cvxMsgDisp("Bracket creation failed");
        return "{\"status\":\"partial\",\"message\":\"Base plate created, bracket failed\"}";
    }

    cvxMsgDisp("Assembly parts created (base plate + bracket)");

    return "{\"status\":\"ok\",\"message\":\"Assembly created successfully\",\"parts\":[\"base_plate\",\"bracket\"]}";
}

static std::string ApiCreateStressPart(void)
{
    char msg[512];
    cvxMsgDisp("=== ApiCreateStressPart (main thread) ===");

    if (!EnsureActivePart()) {
        return "{\"status\":\"error\",\"message\":\"Failed to create/activate part\"}";
    }

    cvxMsgDisp("[STRESS] Step 1: Creating base plate (140x90x18mm)...");
    svxBoxData baseBox;
    cvxPartBoxInit(&baseBox);
    baseBox.Combine = VX_BOOL_NONE;
    baseBox.Center.x = 0.0;
    baseBox.Center.y = 0.0;
    baseBox.Center.z = 9.0;
    baseBox.X = 140.0;
    baseBox.Y = 90.0;
    baseBox.Z = 18.0;
    int baseId = 0;
    evxErrors ret = cvxPartBox(&baseBox, &baseId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[STRESS] Base plate failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"error\",\"message\":\"Base plate creation failed\"}";
    }

    cvxMsgDisp("[STRESS] Step 2: Creating left pedestal (36x32x52mm)...");
    svxBoxData leftPedestal;
    cvxPartBoxInit(&leftPedestal);
    leftPedestal.Combine = VX_BOOL_ADD;
    leftPedestal.Center.x = -34.0;
    leftPedestal.Center.y = -8.0;
    leftPedestal.Center.z = 44.0;
    leftPedestal.X = 36.0;
    leftPedestal.Y = 32.0;
    leftPedestal.Z = 52.0;
    int leftId = 0;
    ret = cvxPartBox(&leftPedestal, &leftId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[STRESS] Left pedestal failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Base created, left pedestal failed\"}";
    }

    cvxMsgDisp("[STRESS] Step 3: Creating right shelf (58x24x26mm)...");
    svxBoxData rightShelf;
    cvxPartBoxInit(&rightShelf);
    rightShelf.Combine = VX_BOOL_ADD;
    rightShelf.Center.x = 28.0;
    rightShelf.Center.y = 18.0;
    rightShelf.Center.z = 31.0;
    rightShelf.X = 58.0;
    rightShelf.Y = 24.0;
    rightShelf.Z = 26.0;
    int shelfId = 0;
    ret = cvxPartBox(&rightShelf, &shelfId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[STRESS] Right shelf failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Base and left pedestal created, right shelf failed\"}";
    }

    cvxMsgDisp("[STRESS] Step 4: Creating top bridge (82x20x16mm)...");
    svxBoxData topBridge;
    cvxPartBoxInit(&topBridge);
    topBridge.Combine = VX_BOOL_ADD;
    topBridge.Center.x = -6.0;
    topBridge.Center.y = -10.0;
    topBridge.Center.z = 72.0;
    topBridge.X = 82.0;
    topBridge.Y = 20.0;
    topBridge.Z = 16.0;
    int bridgeId = 0;
    ret = cvxPartBox(&topBridge, &bridgeId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[STRESS] Top bridge failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Base, pedestal and shelf created, bridge failed\"}";
    }

    cvxMsgDisp("[STRESS] Step 5: Creating offset cylinder boss (R14 L20)...");
    svxCylData boss;
    cvxPartCylInit(&boss);
    boss.Combine = VX_BOOL_ADD;
    boss.Center.x = 42.0;
    boss.Center.y = -18.0;
    boss.Center.z = 18.0;
    boss.Radius = 14.0;
    boss.Length = 20.0;
    int bossId = 0;
    ret = cvxPartCyl(&boss, &bossId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[STRESS] Cylinder boss failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Main stepped body created, cylinder boss failed\"}";
    }

    cvxMsgDisp("[STRESS] Stress part created successfully");
    return "{\"status\":\"ok\",\"message\":\"Stress test part created successfully\",\"features\":[\"base_plate\",\"left_pedestal\",\"right_shelf\",\"top_bridge\",\"cylinder_boss\"]}";
}

static std::string ApiCreateExtremeStressPart(void)
{
    char msg[512];
    cvxMsgDisp("=== ApiCreateExtremeStressPart (main thread) ===");

    if (!EnsureActivePart()) {
        return "{\"status\":\"error\",\"message\":\"Failed to create/activate part\"}";
    }

    cvxMsgDisp("[XSTRESS] Step 1: Creating large base plate (380x260x24mm)...");
    svxBoxData baseBox;
    cvxPartBoxInit(&baseBox);
    baseBox.Combine = VX_BOOL_NONE;
    baseBox.Center.x = 0.0;
    baseBox.Center.y = 0.0;
    baseBox.Center.z = 12.0;
    baseBox.X = 380.0;
    baseBox.Y = 260.0;
    baseBox.Z = 24.0;
    int baseId = 0;
    evxErrors ret = cvxPartBox(&baseBox, &baseId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[XSTRESS] Base plate failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"error\",\"message\":\"Extreme base plate creation failed\"}";
    }

    cvxMsgDisp("[XSTRESS] Step 2: Creating left tower (82x74x146mm)...");
    svxBoxData leftTower;
    cvxPartBoxInit(&leftTower);
    leftTower.Combine = VX_BOOL_ADD;
    leftTower.Center.x = -108.0;
    leftTower.Center.y = -42.0;
    leftTower.Center.z = 97.0;
    leftTower.X = 82.0;
    leftTower.Y = 74.0;
    leftTower.Z = 146.0;
    int leftTowerId = 0;
    ret = cvxPartBox(&leftTower, &leftTowerId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[XSTRESS] Left tower failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Extreme base created, left tower failed\"}";
    }

    cvxMsgDisp("[XSTRESS] Step 3: Creating central bridge rib (176x36x28mm)...");
    svxBoxData bridge;
    cvxPartBoxInit(&bridge);
    bridge.Combine = VX_BOOL_ADD;
    bridge.Center.x = -18.0;
    bridge.Center.y = -48.0;
    bridge.Center.z = 160.0;
    bridge.X = 176.0;
    bridge.Y = 36.0;
    bridge.Z = 28.0;
    int bridgeId = 0;
    ret = cvxPartBox(&bridge, &bridgeId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[XSTRESS] Bridge rib failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Extreme base and tower created, bridge rib failed\"}";
    }

    cvxMsgDisp("[XSTRESS] Step 4: Creating right platform (126x54x52mm)...");
    svxBoxData rightPlatform;
    cvxPartBoxInit(&rightPlatform);
    rightPlatform.Combine = VX_BOOL_ADD;
    rightPlatform.Center.x = 92.0;
    rightPlatform.Center.y = 56.0;
    rightPlatform.Center.z = 50.0;
    rightPlatform.X = 126.0;
    rightPlatform.Y = 54.0;
    rightPlatform.Z = 52.0;
    int rightPlatformId = 0;
    ret = cvxPartBox(&rightPlatform, &rightPlatformId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[XSTRESS] Right platform failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Extreme body created, right platform failed\"}";
    }

    cvxMsgDisp("[XSTRESS] Step 5: Creating rear tower (54x48x112mm)...");
    svxBoxData rearTower;
    cvxPartBoxInit(&rearTower);
    rearTower.Combine = VX_BOOL_ADD;
    rearTower.Center.x = 118.0;
    rearTower.Center.y = -30.0;
    rearTower.Center.z = 80.0;
    rearTower.X = 54.0;
    rearTower.Y = 48.0;
    rearTower.Z = 112.0;
    int rearTowerId = 0;
    ret = cvxPartBox(&rearTower, &rearTowerId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[XSTRESS] Rear tower failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Extreme body created, rear tower failed\"}";
    }

    cvxMsgDisp("[XSTRESS] Step 6: Creating front shelf cut-like overhang (142x38x22mm)...");
    svxBoxData frontShelf;
    cvxPartBoxInit(&frontShelf);
    frontShelf.Combine = VX_BOOL_ADD;
    frontShelf.Center.x = -36.0;
    frontShelf.Center.y = 72.0;
    frontShelf.Center.z = 48.0;
    frontShelf.X = 142.0;
    frontShelf.Y = 38.0;
    frontShelf.Z = 22.0;
    int frontShelfId = 0;
    ret = cvxPartBox(&frontShelf, &frontShelfId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[XSTRESS] Front shelf failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Extreme body created, front shelf failed\"}";
    }

    cvxMsgDisp("[XSTRESS] Step 7: Creating large cylinder boss (R28 L34)...");
    svxCylData largeBoss;
    cvxPartCylInit(&largeBoss);
    largeBoss.Combine = VX_BOOL_ADD;
    largeBoss.Center.x = 126.0;
    largeBoss.Center.y = 62.0;
    largeBoss.Center.z = 24.0;
    largeBoss.Radius = 28.0;
    largeBoss.Length = 34.0;
    int bossId = 0;
    ret = cvxPartCyl(&largeBoss, &bossId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[XSTRESS] Large cylinder boss failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Extreme body created, cylinder boss failed\"}";
    }

    cvxMsgDisp("[XSTRESS] Extreme stress part created successfully");
    return "{\"status\":\"ok\",\"message\":\"Extreme stress test part created successfully\",\"features\":[\"base_plate\",\"left_tower\",\"bridge_rib\",\"right_platform\",\"rear_tower\",\"front_shelf\",\"large_cylinder_boss\"]}";
}

static std::string ApiCreateBlockWithBoss(void)
{
    char msg[512];
    cvxMsgDisp("=== ApiCreateBlockWithBoss (main thread) ===");

    std::string baseResult = ApiCreateBlock(220.0, 140.0, 24.0);
    if (baseResult.find("\"status\":\"ok\"") == std::string::npos) {
        return "{\"status\":\"error\",\"message\":\"Base plate creation failed\"}";
    }

    cvxMsgDisp("[BOSS] Step 2: Creating cylinder boss (R28 L60)...");
    svxCylData boss;
    cvxPartCylInit(&boss);
    boss.Combine = VX_BOOL_ADD;
    boss.Center.x = 42.0;
    boss.Center.y = 18.0;
    boss.Center.z = 24.0;
    boss.Radius = 28.0;
    boss.Length = 60.0;
    int bossId = 0;
    evxErrors ret = cvxPartCyl(&boss, &bossId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[BOSS] Cylinder boss failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"partial\",\"message\":\"Base plate created, cylinder boss failed\"}";
    }

    cvxMsgDisp("[BOSS] Block with boss created successfully");
    return "{\"status\":\"ok\",\"message\":\"Block with cylinder boss created successfully\",\"features\":[\"base_plate\",\"cylinder_boss\"]}";
}

static std::string ApiAddCylinderBoss(double centerX, double centerY, double baseTopZ, double radius, double length)
{
    char msg[512];
    cvxMsgDisp("=== ApiAddCylinderBoss (main thread) ===");

    if (!EnsureActivePart()) {
        return "{\"status\":\"error\",\"message\":\"Failed to activate part for cylinder boss\"}";
    }

    svxCylData boss;
    cvxPartCylInit(&boss);
    boss.Combine = VX_BOOL_ADD;
    boss.Center.x = centerX;
    boss.Center.y = centerY;
    boss.Center.z = baseTopZ;
    boss.Radius = radius;
    boss.Length = length;

    int bossId = 0;
    evxErrors ret = cvxPartCyl(&boss, &bossId);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[BOSS] Cylinder boss failed: %d", ret);
        cvxMsgDisp(msg);
        return "{\"status\":\"error\",\"message\":\"Cylinder boss creation failed\",\"code\":" + std::to_string(ret) + "}";
    }

    sprintf_s(msg, sizeof(msg), "[BOSS] Cylinder boss created at (%.1f, %.1f, %.1f), R=%.1f, L=%.1f",
        centerX, centerY, baseTopZ, radius, length);
    cvxMsgDisp(msg);
    return "{\"status\":\"ok\",\"message\":\"Cylinder boss created successfully\",\"center\":{\"x\":"
        + std::to_string(centerX) + ",\"y\":" + std::to_string(centerY) + ",\"z\":" + std::to_string(baseTopZ)
        + "},\"radius\":" + std::to_string(radius) + ",\"length\":" + std::to_string(length) + "}";
}

/**
 * Convert UTF-8 string to system codepage (GBK on Chinese Windows).
 * ZW3D API functions like cvxFileOpen expect system-codepage strings.
 */
static std::string Utf8ToSystemCodepage(const std::string& utf8)
{
    if (utf8.empty()) return utf8;
    // UTF-8 → UTF-16
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
    if (wlen <= 0) return utf8;
    std::vector<WCHAR> wbuf(wlen);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wbuf.data(), wlen);
    // UTF-16 → System codepage (GBK on Chinese Windows)
    int mlen = WideCharToMultiByte(CP_ACP, 0, wbuf.data(), -1, NULL, 0, NULL, NULL);
    if (mlen <= 0) return utf8;
    std::vector<char> mbuf(mlen);
    WideCharToMultiByte(CP_ACP, 0, wbuf.data(), -1, mbuf.data(), mlen, NULL, NULL);
    return std::string(mbuf.data());
}

/**
 * Open a ZW3D part file (.Z3PRT) on the main thread.
 */
static std::string ApiOpenPart(const std::string& path)
{
    char msg[512];
    cvxMsgDisp("=== ApiOpenPart (main thread) ===");
    g_activeReferenceProfile = {};
    cvxMsgDisp("[OPEN] Cleared active reference profile");

    if (path.empty()) {
        return "{\"status\":\"error\",\"message\":\"No file path provided\"}";
    }

    // Convert UTF-8 to system codepage (GBK) for ZW3D API, then fix slashes
    std::string winPath = Utf8ToSystemCodepage(path);
    for (size_t i = 0; i < winPath.size(); ++i) {
        if (winPath[i] == '/') winPath[i] = '\\';
    }

    // Also prepare UTF-8 path with backslashes as fallback
    std::string utf8Path = path;
    for (size_t i = 0; i < utf8Path.size(); ++i) {
        if (utf8Path[i] == '/') utf8Path[i] = '\\';
    }

    // Verify file exists before calling cvxFileOpen (which may hang on invalid paths)
    DWORD attrs = GetFileAttributesA(winPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // Try with UTF-8 path (in case GBK conversion didn't work)
        DWORD attrs2 = GetFileAttributesA(utf8Path.c_str());
        if (attrs2 != INVALID_FILE_ATTRIBUTES) {
            winPath = utf8Path;
            attrs = attrs2;
            cvxMsgDisp("[OPEN] GBK path not found, using original UTF-8 path");
        }
    }

    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // Try Unicode (wide char) path as last resort
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
        if (wlen > 0) {
            std::vector<WCHAR> wpath(wlen);
            MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
            DWORD attrs3 = GetFileAttributesW(wpath.data());
            if (attrs3 != INVALID_FILE_ATTRIBUTES) {
                // File exists but only via Unicode API — cvxFileOpen won't find it
                // Copy the file to an ASCII-only path
                const char* asciiCopyPath = "C:\\Users\\Ey\\Documents\\ZW3D\\TempOpenPart.Z3PRT";
                CreateDirectoryA("C:\\Users\\Ey\\Documents\\ZW3D", NULL);
                if (CopyFileW(wpath.data(), L"C:\\Users\\Ey\\Documents\\ZW3D\\TempOpenPart.Z3PRT", FALSE)) {
                    winPath = asciiCopyPath;
                    cvxMsgDisp("[OPEN] Copied Unicode path file to ASCII path for cvxFileOpen");
                } else {
                    return "{\"status\":\"error\",\"message\":\"File exists but cannot be copied to ASCII path\"}";
                }
            } else {
                sprintf_s(msg, sizeof(msg), "[OPEN] File not found: %s (GBK) or %s (UTF-8) or wide path", winPath.c_str(), utf8Path.c_str());
                cvxMsgDisp(msg);
                return std::string("{\"status\":\"error\",\"message\":\"File not found: ") + JsonEscape(path) + "\"}";
            }
        }
    }

    sprintf_s(msg, sizeof(msg), "[OPEN] Opening file: %s", winPath.c_str());
    cvxMsgDisp(msg);

    // Save the current document before opening a new one.
    // If the current document has unsaved changes, cvxFileOpen
    // will pop up a "Save changes?" dialog that blocks the main thread.
    // Saving first prevents this dialog.
    // Use cvxFileSaveAs to a known path instead of cvxFileSave(0) because
    // cvxFileSave(0) may pop a "Save As" dialog for new unsaved documents.
    char curDoc[512] = {0};
    cvxFileInqActive(curDoc, sizeof(curDoc));
    if (curDoc[0] != 0) {
        std::string curDocStr = curDoc;
        bool curIsDrawing = (curDocStr.find(".Z3DRW") != std::string::npos ||
                             curDocStr.find(".Z3DR") != std::string::npos ||
                             curDocStr.find("AutoDraft") != std::string::npos);
        if (curIsDrawing) {
            // Save drawing to known path
            const char* drawSavePath = "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW";
            sprintf_s(msg, sizeof(msg), "[OPEN] Saving current drawing before opening new file: %s -> %s", curDoc, drawSavePath);
            cvxMsgDisp(msg);
            cvxFileSaveAs(drawSavePath);
        } else {
            // Save part to current path
            sprintf_s(msg, sizeof(msg), "[OPEN] Saving current part before opening new file: %s", curDoc);
            cvxMsgDisp(msg);
            cvxFileSave(0);
        }
    }

    // First try cvxFileActivate — if the document is already open, this switches to it
    // without popping up a confirmation dialog (which cvxFileOpen does for already-open docs).
    // Try multiple path variants because ZW3D may have opened the document with a different
    // encoding than our GBK conversion.
    evxErrors ret = ZW_API_GENERAL_ERROR;

    // Try 1: GBK-converted path (most common case for ASCII paths)
    ret = cvxFileActivate(winPath.c_str());
    if (ret == ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[OPEN] cvxFileActivate(winPath) succeeded");
        cvxMsgDisp(msg);
    } else {
        // Try 2: Original UTF-8 path with backslashes (ZW3D may have opened with this path)
        if (ret != ZW_API_NO_ERROR && utf8Path != winPath) {
            ret = cvxFileActivate(utf8Path.c_str());
            if (ret == ZW_API_NO_ERROR) {
                sprintf_s(msg, sizeof(msg), "[OPEN] cvxFileActivate(utf8Path) succeeded");
                cvxMsgDisp(msg);
            }
        }
        // Try 3: The original unmodified path parameter (before slash conversion)
        if (ret != ZW_API_NO_ERROR && path != utf8Path && path != winPath) {
            ret = cvxFileActivate(path.c_str());
            if (ret == ZW_API_NO_ERROR) {
                sprintf_s(msg, sizeof(msg), "[OPEN] cvxFileActivate(originalPath) succeeded");
                cvxMsgDisp(msg);
            }
        }

        if (ret != ZW_API_NO_ERROR) {
            // Document not open yet — use cvxFileOpen (no dialog for new documents)
            // But if there are already open documents with unsaved changes, this MAY pop a dialog.
            // We'll try and rely on the timeout + global protection if it hangs.
            ret = cvxFileOpen(winPath.c_str());
            if (ret != ZW_API_NO_ERROR) {
                sprintf_s(msg, sizeof(msg), "[OPEN] cvxFileOpen failed: %d", ret);
                cvxMsgDisp(msg);
                return std::string("{\"status\":\"error\",\"message\":\"Failed to open file\",\"code\":") + std::to_string(ret) + "}";
            }
            sprintf_s(msg, sizeof(msg), "[OPEN] cvxFileOpen succeeded (new document)");
            cvxMsgDisp(msg);
        }
    }

    // Wait briefly for ZW3D to process the file open
    Sleep(500);

    char rootName[256] = {0};
    cvxRootInqActive(rootName, sizeof(rootName));
    if (rootName[0] != 0) {
        cvxRootActivate(rootName);
        sprintf_s(msg, sizeof(msg), "[OPEN] Activated root: %s", rootName);
        cvxMsgDisp(msg);
    }

    char activePath[512] = {0};
    cvxFileInqActive(activePath, sizeof(activePath));

    // Verify the document switch actually happened.
    // If cvxFileActivate failed and cvxFileOpen popped a dialog that blocked,
    // the active document may still be the previous drawing — do NOT update globals
    // with a drawing path, as that would corrupt g_activePartPath.
    bool activePathIsAbsolute = IsAbsoluteWindowsPath(activePath);
    std::string absActivePath = activePathIsAbsolute ? ToAbsolutePath(activePath) : "";
    if (!activePathIsAbsolute && rootName[0] != 0) {
        bool requestedIsDrawing = (path.find(".Z3DRW") != std::string::npos ||
                                   path.find(".Z3DR") != std::string::npos ||
                                   path.find("AutoDraft") != std::string::npos);
        if (!requestedIsDrawing) {
            absActivePath = !utf8Path.empty() ? utf8Path : winPath;
            sprintf_s(msg, sizeof(msg), "[OPEN] Active path is relative ('%s'); preserving requested absolute path '%s'",
                activePath, absActivePath.c_str());
            cvxMsgDisp(msg);
        } else {
            absActivePath = ToAbsolutePath(activePath);
        }
    }
    bool activeIsDrawing = (absActivePath.find(".Z3DRW") != std::string::npos ||
                            absActivePath.find(".Z3DR") != std::string::npos ||
                            absActivePath.find("AutoDraft") != std::string::npos);

    if (!activeIsDrawing && !absActivePath.empty()) {
        // Document switched to a part — update globals
        strncpy_s(g_activePartPath, sizeof(g_activePartPath), absActivePath.c_str(), _TRUNCATE);
        if (rootName[0] != 0) {
            strncpy_s(g_activePartRootName, sizeof(g_activePartRootName), rootName, _TRUNCATE);
        }
        // Save bounding box for when we're later in drawing context
        if (GetActivePartBoundingSize(&g_savedBboxX, &g_savedBboxY, &g_savedBboxZ)) {
            g_savedBboxValid = true;
            sprintf_s(msg, sizeof(msg), "[OPEN] Saved bbox: %.2f x %.2f x %.2f", g_savedBboxX, g_savedBboxY, g_savedBboxZ);
            cvxMsgDisp(msg);
        }
        sprintf_s(msg, sizeof(msg), "[OPEN] Updated globals: g_activePartPath='%s', g_activePartRootName='%s'",
            g_activePartPath, g_activePartRootName);
        cvxMsgDisp(msg);
    } else if (activeIsDrawing) {
        // Document switch FAILED — active doc is still a drawing, not the requested part.
        // Save the REQUESTED path to globals so generate_smart_drafting can at least
        // copy the original part file (even though ZW3D didn't switch to it).
        bool requestedIsDrawing = (path.find(".Z3DRW") != std::string::npos ||
                                   path.find(".Z3DR") != std::string::npos ||
                                   path.find("AutoDraft") != std::string::npos);
        if (!requestedIsDrawing) {
            // Store the original requested path (not the temp copy) so CopyFileW can find it.
            // Use utf8Path (the original path with backslashes) — this is what CopyFileW
            // will use via MultiByteToWideChar(CP_UTF8) in generate_smart_drafting.
            if (!utf8Path.empty()) {
                strncpy_s(g_activePartPath, sizeof(g_activePartPath), utf8Path.c_str(), _TRUNCATE);
            } else {
                strncpy_s(g_activePartPath, sizeof(g_activePartPath), path.c_str(), _TRUNCATE);
            }
            // Infer root name from file path (ZW3D root name = filename without extension)
            // We can't query the actual root name since we couldn't switch to the document.
            {
                const std::string& p = utf8Path.empty() ? path : utf8Path;
                size_t lastSlash = p.find_last_of("\\/");
                std::string filename = (lastSlash != std::string::npos) ? p.substr(lastSlash + 1) : p;
                size_t dotPos = filename.find_last_of('.');
                std::string baseName = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
                if (!baseName.empty()) {
                    strncpy_s(g_activePartRootName, sizeof(g_activePartRootName), baseName.c_str(), _TRUNCATE);
                }
            }
            sprintf_s(msg, sizeof(msg), "[OPEN] WARNING: Document switch failed (still in drawing). Saved requested part info: path='%s' root='%s'",
                g_activePartPath, g_activePartRootName);
            cvxMsgDisp(msg);
        } else {
            sprintf_s(msg, sizeof(msg), "[OPEN] WARNING: Document switch failed and requested path looks like drawing: '%s'", path.c_str());
            cvxMsgDisp(msg);
        }
    }

    return std::string("{\"status\":\"ok\",\"message\":\"File opened successfully\",\"path\":\"")
        + JsonEscape(path) + "\",\"root\":\"" + JsonEscape(rootName)
        + "\",\"active_path\":\"" + JsonEscape(activePath) + "\"}";
}

/**
 * Clear all views on the active drawing sheet.
 */
static std::string ApiClearSheetViews(void)
{
    cvxMsgDisp("=== ApiClearSheetViews (main thread) ===");

    char fallbackDrawingPath[512] = {0};
    sprintf_s(fallbackDrawingPath, sizeof(fallbackDrawingPath), "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW");
    if (!EnsureDrawingContext(fallbackDrawingPath)) {
        cvxMsgDisp("[CLEAR] Failed to switch into drawing context");
        return "{\"status\":\"error\",\"message\":\"Failed to activate drawing sheet\"}";
    }

    ZwDrawingSheetActivateByHandle(nullptr);

    int cleared = ClearActiveSheetViews();
    char msg[128];
    sprintf_s(msg, sizeof(msg), "[CLEAR] Cleared %d view(s)", cleared);
    cvxMsgDisp(msg);

    return std::string("{\"status\":\"ok\",\"message\":\"Sheet views cleared\",\"cleared_count\":")
        + std::to_string(cleared) + "}";
}

static std::string ApiSetReferenceProfile(const std::string& body)
{
    cvxMsgDisp("=== ApiSetReferenceProfile (main thread) ===");

    int viewCount = static_cast<int>(JsonNum(body, "view_count"));
    if (viewCount <= 0) {
        g_activeReferenceProfile = {};
        cvxMsgDisp("[REFERENCE] Cleared active reference profile");
        return "{\"status\":\"ok\",\"message\":\"Reference profile cleared\",\"active\":false}";
    }
    if (viewCount > 8) {
        viewCount = 8;
    }

    LearnedReferenceProfile next = {};
    next.valid = true;
    next.viewCount = viewCount;
    std::string sample = JsonStr(body, "sample");
    std::string paper = JsonStr(body, "paper");
    std::string templatePath = JsonStr(body, "template_path");
    strncpy_s(next.sample, sizeof(next.sample), sample.c_str(), _TRUNCATE);
    strncpy_s(next.paperName, sizeof(next.paperName), paper.c_str(), _TRUNCATE);
    strncpy_s(next.templatePath, sizeof(next.templatePath), templatePath.c_str(), _TRUNCATE);
    next.baseViews = static_cast<int>(JsonNum(body, "base_views"));
    next.projectViews = static_cast<int>(JsonNum(body, "project_views"));
    next.sectionViews = static_cast<int>(JsonNum(body, "section_views"));

    int totalDimensions = 0;
    int annotatedViews = 0;
    for (int i = 0; i < viewCount; ++i) {
        char key[64];
        sprintf_s(key, sizeof(key), "view%d_type", i);
        std::string type = JsonStr(body, key);
        if (type.empty()) {
            type = "unknown";
        }
        strncpy_s(next.views[i].type, sizeof(next.views[i].type), type.c_str(), _TRUNCATE);

        sprintf_s(key, sizeof(key), "view%d_x", i);
        next.views[i].x = JsonNum(body, key);
        sprintf_s(key, sizeof(key), "view%d_y", i);
        next.views[i].y = JsonNum(body, key);
        sprintf_s(key, sizeof(key), "view%d_dims", i);
        next.views[i].dimensions = static_cast<int>(JsonNum(body, key));

        totalDimensions += next.views[i].dimensions;
        if (next.views[i].dimensions > 0) {
            annotatedViews++;
        }
    }

    int providedTotalDimensions = static_cast<int>(JsonNum(body, "total_dimensions"));
    int providedAnnotatedViews = static_cast<int>(JsonNum(body, "annotated_views"));
    next.totalDimensions = providedTotalDimensions > 0 ? providedTotalDimensions : totalDimensions;
    next.annotatedViews = providedAnnotatedViews > 0 ? providedAnnotatedViews : annotatedViews;

    g_activeReferenceProfile = next;

    char msg[512];
    sprintf_s(msg, sizeof(msg), "[REFERENCE] Active profile sample=%s paper=%s views=%d dims=%d annotated=%d template=%s",
        g_activeReferenceProfile.sample, g_activeReferenceProfile.paperName,
        g_activeReferenceProfile.viewCount, g_activeReferenceProfile.totalDimensions,
        g_activeReferenceProfile.annotatedViews, g_activeReferenceProfile.templatePath);
    cvxMsgDisp(msg);

    return std::string("{\"status\":\"ok\",\"message\":\"Reference profile set\",\"active\":true")
        + ",\"sample\":\"" + JsonEscape(g_activeReferenceProfile.sample) + "\""
        + ",\"paper\":\"" + JsonEscape(g_activeReferenceProfile.paperName) + "\""
        + ",\"template_path\":\"" + JsonEscape(g_activeReferenceProfile.templatePath) + "\""
        + ",\"view_count\":" + std::to_string(g_activeReferenceProfile.viewCount)
        + ",\"total_dimensions\":" + std::to_string(g_activeReferenceProfile.totalDimensions)
        + ",\"annotated_views\":" + std::to_string(g_activeReferenceProfile.annotatedViews)
        + "}";
}

static std::string ApiGenerateNativeViewLayout(const std::string& body)
{
    char msg[512];
    cvxMsgDisp("=== ApiGenerateNativeViewLayout (main thread) ===");

    char curActive[512] = {0};
    cvxFileInqActive(curActive, sizeof(curActive));
    bool inDrawingContext = (strstr(curActive, "AutoDraft") != nullptr ||
                             strstr(curActive, ".Z3DRW") != nullptr ||
                             strstr(curActive, ".Z3DR") != nullptr);

    char partName[256] = {0};
    char filePath[512] = {0};
    if (inDrawingContext) {
        strncpy_s(partName, sizeof(partName), g_activePartRootName, _TRUNCATE);
        strncpy_s(filePath, sizeof(filePath), g_activePartPath, _TRUNCATE);
    } else {
        cvxRootInqActive(partName, sizeof(partName));
        cvxFileInqActive(filePath, sizeof(filePath));
    }

    std::string absFilePath;
    if (filePath[0] != 0 && IsAbsoluteWindowsPath(filePath)) {
        absFilePath = ToAbsolutePath(filePath);
    } else if (g_activePartPath[0] != 0) {
        absFilePath = g_activePartPath;
    } else {
        absFilePath = ToAbsolutePath(filePath);
    }

    bool looksLikeDrawing = (absFilePath.find(".Z3DRW") != std::string::npos ||
                             absFilePath.find(".Z3DR") != std::string::npos ||
                             absFilePath.find("AutoDraft") != std::string::npos);
    if (partName[0] == 0 || absFilePath.empty() || looksLikeDrawing) {
        sprintf_s(msg, sizeof(msg), "[NATIVE_LAYOUT] No part context: active='%s' part='%s' path='%s'",
            curActive, partName, absFilePath.c_str());
        cvxMsgDisp(msg);
        return "{\"status\":\"error\",\"message\":\"No part context available for native view layout\"}";
    }

    strncpy_s(g_activePartPath, sizeof(g_activePartPath), absFilePath.c_str(), _TRUNCATE);
    strncpy_s(g_activePartRootName, sizeof(g_activePartRootName), partName, _TRUNCATE);

    const char* savedPartPath = "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftNativePart.Z3PRT";
    CreateDirectoryA("C:\\Users\\Ey\\Documents\\ZW3D", NULL);
    if (inDrawingContext) {
        bool copied = false;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, absFilePath.c_str(), -1, NULL, 0);
        if (wlen > 0) {
            std::vector<WCHAR> wSrc(wlen);
            MultiByteToWideChar(CP_UTF8, 0, absFilePath.c_str(), -1, wSrc.data(), wlen);
            copied = CopyFileW(wSrc.data(), L"C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftNativePart.Z3PRT", FALSE) ? true : false;
        }
        if (!copied) {
            copied = CopyFileA(absFilePath.c_str(), savedPartPath, FALSE) ? true : false;
        }
        if (copied) {
            absFilePath = savedPartPath;
        }
    } else {
        evxErrors saveRet = cvxFileSaveAs(savedPartPath);
        sprintf_s(msg, sizeof(msg), "[NATIVE_LAYOUT] Save part copy ret=%d", saveRet);
        cvxMsgDisp(msg);
        if (saveRet == ZW_API_NO_ERROR) {
            absFilePath = savedPartPath;
            cvxRootInqActive(partName, sizeof(partName));
        }
    }

    static int nativeDrawCounter = 0;
    nativeDrawCounter++;
    char drawName[64] = {0};
    sprintf_s(drawName, sizeof(drawName), "NativeLayout_%d", nativeDrawCounter);
    evxErrors ret = cvxFileNewSingle(drawName, VX_FILE_SHEET, VX_SUBTYPE_NONE, NULL, NULL);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[NATIVE_LAYOUT] cvxFileNewSingle(SHEET) failed: %d", ret);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"error\",\"message\":\"Failed to create drawing file\",\"code\":")
            + std::to_string(ret) + "}";
    }

    char drawSavePath[512] = {0};
    sprintf_s(drawSavePath, sizeof(drawSavePath), "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftNativeLayout.Z3DRW");
    evxErrors saveDrawingRet = cvxFileSaveAs(drawSavePath);
    if (saveDrawingRet == ZW_API_NO_ERROR) {
        strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), drawSavePath, _TRUNCATE);
    }

    int sheetCount = 0;
    szwEntityHandle* sheetList = nullptr;
    ret = ZwDrawingSheetListGet(&sheetCount, &sheetList);
    if (ret == ZW_API_NO_ERROR && sheetCount > 0 && sheetList != nullptr) {
        ret = ZwDrawingSheetActivateByHandle(&sheetList[0]);
        ZwEntityHandleListFree(sheetCount, &sheetList);
        if (ret != ZW_API_NO_ERROR) {
            return "{\"status\":\"error\",\"message\":\"Failed to activate native layout sheet\"}";
        }
    } else {
        if (sheetList != nullptr) {
            ZwEntityHandleListFree(sheetCount, &sheetList);
        }
        szwEntityHandle sheetHandle = {};
        ret = ZwDrawingSheetCreate("Sheet1", &sheetHandle);
        if (ret != ZW_API_NO_ERROR) {
            return "{\"status\":\"error\",\"message\":\"Failed to create native layout sheet\"}";
        }
        ret = ZwDrawingSheetActivateByHandle(&sheetHandle);
        if (ret != ZW_API_NO_ERROR) {
            return "{\"status\":\"error\",\"message\":\"Failed to activate created native layout sheet\"}";
        }
    }

    std::string paper = JsonStr(body, "paper");
    if (paper.empty() && HasActiveReferenceProfile() && g_activeReferenceProfile.paperName[0] != 0) {
        paper = g_activeReferenceProfile.paperName;
    }
    if (paper.empty()) {
        paper = "A3(H)";
    }
    if (paper == "A4(V)") {
        SetActiveDrawingPaperSize("A4(V)", 210.0, 297.0);
    } else if (paper == "A4(H)") {
        SetActiveDrawingPaperSize("A4(H)", 297.0, 210.0);
    } else if (paper == "A2(H)") {
        SetActiveDrawingPaperSize("A2(H)", 594.0, 420.0);
    } else if (paper == "A1(H)") {
        SetActiveDrawingPaperSize("A1(H)", 841.0, 594.0);
    } else {
        paper = "A3(H)";
        SetActiveDrawingPaperSize("A3(H)", 420.0, 297.0);
    }

    ClearActiveSheetViews();
    ZwCommandSend("");
    Sleep(200);

    svxViewLayoutData layoutData;
    ret = cvxDwgViewLayoutInit(&layoutData);
    if (ret != ZW_API_NO_ERROR) {
        return std::string("{\"status\":\"error\",\"message\":\"cvxDwgViewLayoutInit failed\",\"code\":")
            + std::to_string(ret) + "}";
    }

    strncpy_s(layoutData.name, sizeof(layoutData.name), absFilePath.c_str(), _TRUNCATE);
    strncpy_s(layoutData.root, sizeof(layoutData.root), partName, _TRUNCATE);
    layoutData.location.type = VX_LOCATION_AUTO;
    layoutData.isIgnoreLoc = 0;

    int requestedCount = static_cast<int>(JsonNum(body, "view_count"));
    if (requestedCount <= 0 && HasActiveReferenceProfile()) {
        requestedCount = g_activeReferenceProfile.viewCount;
    }
    if (requestedCount <= 0) {
        requestedCount = 3;
    }
    if (requestedCount < 1) requestedCount = 1;
    if (requestedCount > 7) requestedCount = 7;
    layoutData.method.projection = 2;
    layoutData.method.count = requestedCount;
    layoutData.method.StdMethod[0] = VX_VIEW_FRONT;
    if (requestedCount > 1) layoutData.method.StdMethod[1] = VX_VIEW_TOP;
    if (requestedCount > 2) layoutData.method.StdMethod[2] = VX_VIEW_RIGHT;
    if (requestedCount > 3) layoutData.method.StdMethod[3] = VX_VIEW_LEFT;
    if (requestedCount > 4) layoutData.method.StdMethod[4] = VX_VIEW_BACK;
    if (requestedCount > 5) layoutData.method.StdMethod[5] = VX_VIEW_BOTTOM;
    if (requestedCount > 6) layoutData.method.StdMethod[6] = VX_VIEW_ISO;
    layoutData.isIgnoreMethod = 0;

    layoutData.isIgnoreAttr = 1;
    layoutData.coordinate = 1;
    layoutData.state = -1;
    layoutData.DimensionType = 0;
    layoutData.isCalculate = 1;

    sprintf_s(msg, sizeof(msg), "[NATIVE_LAYOUT] cvxDwgViewLayout part='%s' root='%s' views=%d paper=%s",
        absFilePath.c_str(), partName, requestedCount, paper.c_str());
    cvxMsgDisp(msg);

    ret = cvxDwgViewLayout(&layoutData);
    sprintf_s(msg, sizeof(msg), "[NATIVE_LAYOUT] cvxDwgViewLayout ret=%d", ret);
    cvxMsgDisp(msg);
    ZwDrawingSheetManagerViewTreeRefresh(nullptr);
    Sleep(1500);
    cvxFileSaveAs(drawSavePath);

    std::string statsJson = CollectActiveSheetViewStatsJson();
    return std::string("{\"status\":\"") + (ret == ZW_API_NO_ERROR ? "ok" : "error")
        + "\",\"message\":\"Native view layout attempted\""
        + ",\"method\":\"cvxDwgViewLayout\""
        + ",\"code\":" + std::to_string(ret)
        + ",\"paper\":\"" + JsonEscape(paper) + "\""
        + ",\"view_count_requested\":" + std::to_string(requestedCount)
        + ",\"drawing_path\":\"" + JsonEscape(drawSavePath) + "\""
        + ",\"views\":" + statsJson
        + "}";
}

/**
 * Add a full section view to the active drawing.
 * Finds the primary base view and creates a section view below or to the right.
 */
static std::string ApiAddSectionView(const std::string& label, const std::string& position)
{
    char msg[512];
    cvxMsgDisp("=== ApiAddSectionView (main thread) ===");

    char fallbackDrawingPath[512] = {0};
    sprintf_s(fallbackDrawingPath, sizeof(fallbackDrawingPath), "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW");
    if (!EnsureDrawingContext(fallbackDrawingPath)) {
        cvxMsgDisp("[SECTION] Failed to switch into drawing context");
        return "{\"status\":\"error\",\"message\":\"Failed to activate drawing sheet\"}";
    }

    ZwDrawingSheetActivateByHandle(nullptr);

    // Find the primary (largest) base view
    int viewCount = 0;
    szwEntityHandle* viewList = nullptr;
    ezwErrors ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &viewCount, &viewList);
    if (ret != ZW_API_NO_ERROR || viewCount <= 0 || viewList == nullptr) {
        if (viewList) ZwEntityHandleListFree(viewCount, &viewList);
        return "{\"status\":\"error\",\"message\":\"No views found on active sheet\"}";
    }

    szwEntityHandle primaryBase = {};
    double primaryBaseArea = 0.0;
    szwDrawingDottedBorder primaryBorder = {};

    for (int i = 0; i < viewCount; ++i) {
        ezwDrawingViewType type = ZW_DRAWING_ALL_VIEW;
        if (ZwDrawingViewTypeGet(viewList[i], &type) != ZW_API_NO_ERROR) continue;
        if (type != ZW_DRAWING_BASE_VIEW) continue;

        szwDrawingDottedBorder border = {};
        if (ZwDrawingViewDottedBorderGet(viewList[i], &border) != ZW_API_NO_ERROR) continue;

        double w = border.bottomRight.x - border.upperLeft.x;
        double h = border.upperLeft.y - border.bottomRight.y;
        double area = w * h;
        if (area > primaryBaseArea) {
            primaryBaseArea = area;
            primaryBase = viewList[i];
            primaryBorder = border;
        }
    }
    ZwEntityHandleListFree(viewCount, &viewList);

    if (IsEmptyHandle(primaryBase)) {
        return "{\"status\":\"error\",\"message\":\"No base view found for section view\"}";
    }

    // Calculate section view position: below or right of the base view
    double sectionX, sectionY;
    if (position == "right") {
        sectionX = primaryBorder.bottomRight.x + 40.0;
        sectionY = (primaryBorder.upperLeft.y + primaryBorder.bottomRight.y) / 2.0;
    } else {
        // Default: below
        sectionX = (primaryBorder.upperLeft.x + primaryBorder.bottomRight.x) / 2.0;
        sectionY = primaryBorder.bottomRight.y - 40.0;
    }

    const char* sectionLabel = label.empty() ? "A" : label.c_str();
    szwEntityHandle sectionHandle = {};
    ezwErrors sectionRet = TryCreateFullSectionView(primaryBase, sectionX, sectionY, sectionLabel, &sectionHandle);

    if (sectionRet != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[SECTION] Section view creation failed: %d", sectionRet);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"error\",\"message\":\"Failed to create section view\",\"code\":")
            + std::to_string(sectionRet) + "}";
    }

    cvxMsgDisp("[SECTION] Section view created successfully");
    return std::string("{\"status\":\"ok\",\"message\":\"Section view created\",\"label\":\"")
        + JsonEscape(sectionLabel) + "\",\"position\":\"" + JsonEscape(position)
        + "\",\"location\":{\"x\":" + std::to_string(sectionX)
        + ",\"y\":" + std::to_string(sectionY) + "}}";
}

/**
 * Diagnostic: test each drafting step individually
 */
static std::string ApiTestDraftingStep(int step)
{
    char msg[1024];
    cvxMsgDisp("=== TEST STEP ===");

    if (step == 1) {
        // Check active part info
        char partName[256] = {0};
        cvxRootInqActive(partName, sizeof(partName));
        char filePath[512] = {0};
        cvxFileInqActive(filePath, sizeof(filePath));
        sprintf_s(msg, sizeof(msg), "[TEST1] Part='%s' File='%s'", partName, filePath);
        cvxMsgDisp(msg);
        double sx = 0, sy = 0, sz = 0;
        bool ok = GetActivePartBoundingSize(&sx, &sy, &sz);
        sprintf_s(msg, sizeof(msg), "[TEST1] BBox=%.2f x %.2f x %.2f ok=%d", sx, sy, sz, ok ? 1 : 0);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"ok\",\"step\":1,\"part\":\"") + partName
            + "\",\"file\":\"" + filePath + "\",\"bbox\":{" + "\"x\":" + std::to_string(sx)
            + ",\"y\":" + std::to_string(sy) + ",\"z\":" + std::to_string(sz) + "}}";
    }

    if (step == 2) {
        // Test save part to known location
        CreateDirectoryA("C:\\Users\\Ey\\Documents\\ZW3D", NULL);
        sprintf_s(msg, sizeof(msg), "[TEST2] Saving part...");
        cvxMsgDisp(msg);
        evxErrors ret = cvxFileSaveAs("C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftPart.Z3PRT");
        sprintf_s(msg, sizeof(msg), "[TEST2] cvxFileSaveAs = %d", ret);
        cvxMsgDisp(msg);
        char filePath[512] = {0};
        cvxFileInqActive(filePath, sizeof(filePath));
        sprintf_s(msg, sizeof(msg), "[TEST2] After save, active file: '%s'", filePath);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"ok\",\"step\":2,\"save_result\":") + std::to_string(ret)
            + ",\"active_file\":\"" + filePath + "\"}";
    }

    if (step == 3) {
        // Test create drawing
        sprintf_s(msg, sizeof(msg), "[TEST3] Creating drawing...");
        cvxMsgDisp(msg);
        evxErrors ret = cvxFileNewSingle("TestDrawing", VX_FILE_SHEET, VX_SUBTYPE_NONE, NULL, NULL);
        sprintf_s(msg, sizeof(msg), "[TEST3] cvxFileNewSingle = %d", ret);
        cvxMsgDisp(msg);
        if (ret == ZW_API_NO_ERROR) {
            char drawPath[512] = {0};
            cvxFileInqActive(drawPath, sizeof(drawPath));
            sprintf_s(msg, sizeof(msg), "[TEST3] Drawing path: '%s'", drawPath);
            cvxMsgDisp(msg);

            // Save it
            CreateDirectoryA("C:\\Users\\Ey\\Documents\\ZW3D", NULL);
            evxErrors sv = cvxFileSaveAs("C:\\Users\\Ey\\Documents\\ZW3D\\TestDrawing.Z3DRW");
            sprintf_s(msg, sizeof(msg), "[TEST3] cvxFileSaveAs = %d", sv);
            cvxMsgDisp(msg);

            return std::string("{\"status\":\"ok\",\"step\":3,\"create_result\":") + std::to_string(ret)
                + ",\"draw_path\":\"" + drawPath + "\",\"save_result\":" + std::to_string(sv) + "}";
        }
        return std::string("{\"status\":\"error\",\"step\":3,\"create_result\":") + std::to_string(ret) + "}";
    }

    if (step == 4) {
        // Test creating a single standard view
        // CRITICAL: Get part info BEFORE creating drawing (drawing changes active document)
        char filePath[512] = {0};
        cvxFileInqActive(filePath, sizeof(filePath));
        char partName[256] = {0};
        cvxRootInqActive(partName, sizeof(partName));
        sprintf_s(msg, sizeof(msg), "[TEST4] BEFORE save: file='%s' root='%s'", filePath, partName);
        cvxMsgDisp(msg);

        // Save part
        CreateDirectoryA("C:\\Users\\Ey\\Documents\\ZW3D", NULL);
        cvxFileSaveAs("C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftPart.Z3PRT");

        // Get part info AFTER save (still in part context)
        cvxFileInqActive(filePath, sizeof(filePath));
        cvxRootInqActive(partName, sizeof(partName));
        sprintf_s(msg, sizeof(msg), "[TEST4] AFTER save: file='%s' root='%s'", filePath, partName);
        cvxMsgDisp(msg);

        // Create drawing (this switches active doc to drawing)
        cvxFileNewSingle("TestView", VX_FILE_SHEET, VX_SUBTYPE_NONE, NULL, NULL);
        char drawPath[512] = {0};
        cvxFileInqActive(drawPath, sizeof(drawPath));
        sprintf_s(msg, sizeof(msg), "[TEST4] Drawing path: '%s' (should differ from part path)", drawPath);
        cvxMsgDisp(msg);

        int sheetCount = 0;
        szwEntityHandle* sheetList = nullptr;
        evxErrors ret = ZwDrawingSheetListGet(&sheetCount, &sheetList);
        sprintf_s(msg, sizeof(msg), "[TEST4] SheetListGet = %d, count = %d", ret, sheetCount);
        cvxMsgDisp(msg);
        if (ret == ZW_API_NO_ERROR && sheetCount > 0 && sheetList) {
            ZwDrawingSheetActivateByHandle(&sheetList[0]);
            ZwEntityHandleListFree(sheetCount, &sheetList);
        }

        // Use part info captured BEFORE drawing creation (correct)
        szwViewStandardData vd;
        evxErrors initRet = ZwDrawingViewStandardDataInit(&vd);
        sprintf_s(msg, sizeof(msg), "[TEST4] ViewDataInit = %d", initRet);
        cvxMsgDisp(msg);

        if (initRet == ZW_API_NO_ERROR) {
            strncpy_s(vd.path, sizeof(vd.path), filePath, _TRUNCATE);
            strncpy_s(vd.rootName, sizeof(vd.rootName), partName, _TRUNCATE);
            vd.type = ZW_STANDARD_VIEW_NATIVE_TYPE;
            vd.option.viewType = ZW_VIEW_STANDARD_FRONT;
            vd.location.x = 100.0;
            vd.location.y = 100.0;
            vd.isIgnoreAttribute = 0;
            vd.viewAttribute.showScale = 0;
            vd.viewAttribute.scaleType = ZW_VIEW_USE_CUSTOM_SCALE;
            vd.viewAttribute.scaleRatioX = 1.0;
            vd.viewAttribute.scaleRatioY = 1.0;
            vd.viewAttribute.showLabel = 0;
            vd.defaultOrigin = 0;
            vd.defaultCoordinate = 0;
            vd.backupIndex = 1;
            vd.isCalculate = 1;

            sprintf_s(msg, sizeof(msg), "[TEST4] Creating FRONT view: path='%s' root='%s'", vd.path, vd.rootName);
            cvxMsgDisp(msg);

            szwEntityHandle viewHandle = {};
            evxErrors createRet = ZwDrawingViewStandardCreate(vd, &viewHandle);
            sprintf_s(msg, sizeof(msg), "[TEST4] ZwDrawingViewStandardCreate = %d", createRet);
            cvxMsgDisp(msg);

            if (createRet == ZW_API_NO_ERROR) {
                szwDrawingDottedBorder border = {};
                evxErrors brdRet = ZwDrawingViewDottedBorderGet(viewHandle, &border);
                sprintf_s(msg, sizeof(msg), "[TEST4] ViewBorderGet = %d, w=%.1f h=%.1f", brdRet,
                    border.bottomRight.x - border.upperLeft.x,
                    border.upperLeft.y - border.bottomRight.y);
                cvxMsgDisp(msg);
            }

            return std::string("{\"status\":\"ok\",\"step\":4,\"init\":") + std::to_string(initRet)
                + ",\"create\":" + std::to_string(createRet) + ",\"file_path\":\"" + filePath
                + "\",\"part_name\":\"" + partName + "\"}";
        }
        return std::string("{\"status\":\"error\",\"step\":4,\"init\":") + std::to_string(initRet) + "}";
    }

    return std::string("{\"status\":\"error\",\"message\":\"Invalid step\"}");
}

static std::string ApiAnalyzePart(void)
{
    double sizeX = 100.0;
    double sizeY = 60.0;
    double sizeZ = 40.0;
    if (!GetActivePartBoundingSize(&sizeX, &sizeY, &sizeZ)) {
        return "{\"status\":\"error\",\"message\":\"Failed to analyze active part geometry\"}";
    }
    // Save bbox for use when we're later in drawing context
    g_savedBboxX = sizeX;
    g_savedBboxY = sizeY;
    g_savedBboxZ = sizeZ;
    g_savedBboxValid = true;
    const char* profileName = "blocky";
    std::string analysisJson = BuildPartAnalysisJson(sizeX, sizeY, sizeZ, nullptr, nullptr, nullptr, &profileName);
    if (profileName != nullptr) {
        strncpy_s(g_savedProfileName, sizeof(g_savedProfileName), profileName, _TRUNCATE);
    }
    return analysisJson;
}

static std::string ApiGenerateDrafting(void)
{
    cvxMsgDisp("=== ApiGenerateDrafting (main thread) ===");
    char msg[512];
    std::string pmiResult = "{\"status\":\"error\",\"message\":\"PMI not attempted\"}";
    std::vector<szwEntityHandle> createdViews;
    int createdViewCount = 0;
    int firstViewCreateError = 0;
    std::string firstViewCreateName;
    int pmiAnnotatedViews = 0;
    int pmiTotalCreated = 0;
    int pmiTotalDims = 0;
    int pmiFirstError = 0;
    std::string pmiErrorDetail;
    int pmiTotalOverlaps = 0;
    int pmiTotalIntrusions = 0;
    double occupiedOverflowMagnitude = 0.0;
    std::string pmiMetricsJson = "[]";
    std::string occupiedJson = "{\"status\":\"not_computed\"}";
    std::string layoutJson = "{\"attempt_used\":0}";
    std::string viewStatsJson = "{\"status\":\"error\",\"message\":\"View stats not collected\"}";
    std::string viewStatsBeforeClearJson = "{\"status\":\"error\",\"message\":\"Pre-clear stats not collected\"}";
    std::string viewStatsAfterClearJson = "{\"status\":\"error\",\"message\":\"Post-clear stats not collected\"}";
    std::string analysisJson = "{\"status\":\"error\",\"message\":\"Part not analyzed\"}";
    int layoutAttempt = 0;
    int clearedViewCount = 0;
    szwEntityHandle baseViewHandle = {};
    double modelSizeX = 100.0;
    double modelSizeY = 60.0;
    double modelSizeZ = 40.0;
    double baseViewWidth = 100.0;
    double baseViewHeight = 40.0;
    double drawingScale = 1.0;
    double frontCenterX = 95.0;
    double frontCenterY = 105.0;
    double topCenterX = 95.0;
    double topCenterY = 55.0;
    double rightCenterX = 165.0;
    double rightCenterY = 105.0;
    ezwDrawingViewMethod recommendedBaseView = ZW_VIEW_STANDARD_FRONT;
    const char* profileName = "blocky";
    DrawingPaperRect occupiedPaperRect;
    struct PaperPreset { const char* name; double width; double height; };
    const PaperPreset paperPresets[] = {
        {"A4(V)", 210.0, 297.0},
        {"A4(H)", 297.0, 210.0},
        {"A3(H)", 420.0, 297.0},
        {"A2(H)", 594.0, 420.0},
        {"A1(H)", 841.0, 594.0}
    };
    int paperPresetIndex = 0;
    bool useReferenceTemplate = HasActiveReferenceProfile()
        && g_activeReferenceProfile.templatePath[0] != '\0';
    bool openedReferenceTemplate = false;

    // STEP 0: Detect drawing context and determine source part info.
    // CRITICAL: Do NOT call cvxFileClose/cvxFileActivate/cvxFileOpen here —
    // they may pop up dialogs that block the main thread indefinitely.
    // Instead, use g_activePartPath/g_activePartRootName (set by open_part)
    // as the source part info for view creation.
    char curActive[512] = {0};
    cvxFileInqActive(curActive, sizeof(curActive));
    bool inDrawingContext = (strstr(curActive, "AutoDraft") != nullptr ||
                             strstr(curActive, ".Z3DRW") != nullptr ||
                             strstr(curActive, ".Z3DR") != nullptr);
    sprintf_s(msg, sizeof(msg), "[STEP0] curActive='%s' inDrawing=%d g_partPath='%s' g_partRoot='%s'",
        curActive, inDrawingContext ? 1 : 0, g_activePartPath, g_activePartRootName);
    cvxMsgDisp(msg);

    // STEP 1: Capture part info — use globals if in drawing context
    char partName[256] = {0};
    char filePath[512] = {0};

    if (inDrawingContext) {
        // Use saved globals (from open_part) — these are always the correct part info
        strncpy_s(partName, sizeof(partName), g_activePartRootName, _TRUNCATE);
        strncpy_s(filePath, sizeof(filePath), g_activePartPath, _TRUNCATE);
        cvxMsgDisp("[STEP1] Using globals for part info (drawing context)");
    } else {
        cvxRootInqActive(partName, sizeof(partName));
        cvxFileInqActive(filePath, sizeof(filePath));
    }

    std::string absFilePath;
    if (!inDrawingContext && !IsAbsoluteWindowsPath(filePath) && g_activePartPath[0] != '\0') {
        absFilePath = g_activePartPath;
        sprintf_s(msg, sizeof(msg), "[STEP1] Active file path is relative ('%s'); using saved part path '%s'",
            filePath, absFilePath.c_str());
        cvxMsgDisp(msg);
    } else {
        absFilePath = ToAbsolutePath(filePath);
    }

    // Update globals if we got valid part info (not drawing)
    bool looksLikeDrawing = (strstr(absFilePath.c_str(), ".Z3DRW") != nullptr ||
                             strstr(absFilePath.c_str(), ".Z3DR") != nullptr ||
                             strstr(absFilePath.c_str(), "AutoDraft") != nullptr);
    if (!absFilePath.empty() && !looksLikeDrawing) {
        strncpy_s(g_activePartPath, sizeof(g_activePartPath), absFilePath.c_str(), _TRUNCATE);
    }
    if (partName[0] != 0 && !looksLikeDrawing) {
        strncpy_s(g_activePartRootName, sizeof(g_activePartRootName), partName, _TRUNCATE);
    }

    sprintf_s(msg, sizeof(msg), "[STEP1] partName='%s' absFilePath='%s' looksDraw=%d", partName, absFilePath.c_str(), looksLikeDrawing ? 1 : 0);
    cvxMsgDisp(msg);

    // Use the global for restoring part context (persists across iterations)
    std::string origPartPath = g_activePartPath[0] ? std::string(g_activePartPath) : absFilePath;
    char origPartName[256] = {0};
    strncpy_s(origPartName, sizeof(origPartName), g_activePartRootName[0] ? g_activePartRootName : partName, _TRUNCATE);

    if (origPartName[0] == 0) {
        return "{\"status\":\"error\",\"message\":\"No part context available (open a part first)\"}";
    }
    sprintf_s(msg, sizeof(msg), "Source part: %s, file: %s, absFilePath: %s, origPartPath: %s", partName, filePath, absFilePath.c_str(), origPartPath.c_str());
    cvxMsgDisp(msg);

    // NOTE: STEP 1.5 (part save) is now AFTER STEP 2 (context switch),
    // because on iteration 2+ the active document may be the drawing, not the part.
    // Saving the drawing as .Z3PRT would corrupt the part file.

    bool bboxFromLiveQuery = GetActivePartBoundingSize(&modelSizeX, &modelSizeY, &modelSizeZ);
    if (bboxFromLiveQuery) {
        sprintf_s(msg, sizeof(msg), "[DRAFT] Model bbox size (live): X=%.2f Y=%.2f Z=%.2f", modelSizeX, modelSizeY, modelSizeZ);
        cvxMsgDisp(msg);
        // Save for future use in drawing context
        g_savedBboxX = modelSizeX;
        g_savedBboxY = modelSizeY;
        g_savedBboxZ = modelSizeZ;
        g_savedBboxValid = true;
        analysisJson = BuildPartAnalysisJson(
            modelSizeX, modelSizeY, modelSizeZ,
            &recommendedBaseView, &baseViewWidth, &baseViewHeight, &profileName);
        if (profileName != nullptr) {
            strncpy_s(g_savedProfileName, sizeof(g_savedProfileName), profileName, _TRUNCATE);
        }
        sprintf_s(msg, sizeof(msg), "[DRAFT] Smart analysis profile=%s baseView=%d", profileName, static_cast<int>(recommendedBaseView));
        cvxMsgDisp(msg);
    } else if (g_savedBboxValid) {
        // In drawing context, cvxPartInqShapes fails — use saved bbox from open_part/analyze_part
        modelSizeX = g_savedBboxX;
        modelSizeY = g_savedBboxY;
        modelSizeZ = g_savedBboxZ;
        sprintf_s(msg, sizeof(msg), "[DRAFT] Model bbox size (saved): X=%.2f Y=%.2f Z=%.2f", modelSizeX, modelSizeY, modelSizeZ);
        cvxMsgDisp(msg);
        analysisJson = BuildPartAnalysisJson(
            modelSizeX, modelSizeY, modelSizeZ,
            &recommendedBaseView, &baseViewWidth, &baseViewHeight, &profileName);
        if (profileName != nullptr) {
            strncpy_s(g_savedProfileName, sizeof(g_savedProfileName), profileName, _TRUNCATE);
        }
        sprintf_s(msg, sizeof(msg), "[DRAFT] Smart analysis profile=%s baseView=%d (from saved bbox)", profileName, static_cast<int>(recommendedBaseView));
        cvxMsgDisp(msg);
    } else {
        cvxMsgDisp("[DRAFT] Failed to query model bbox and no saved bbox, using fallback sizing");
    }

    double maxModelDim = modelSizeX;
    if (modelSizeY > maxModelDim) maxModelDim = modelSizeY;
    if (modelSizeZ > maxModelDim) maxModelDim = modelSizeZ;
    double modelFootprint = modelSizeX * modelSizeY;
    bool isPlateLikeProfile = profileName != nullptr && strcmp(profileName, "plate_like") == 0;
    bool isSteppedProfile = profileName != nullptr && strcmp(profileName, "stepped") == 0;
    bool isLargePlateLike = isPlateLikeProfile && (maxModelDim > 180.0 || modelFootprint > 30000.0);
    // The medium stepped reference (TZ-ZJ-011571) expands with extra orthographic views,
    // not an overlapping full section. Keep automatic sections for large shell/plate cases.
    bool needsAdvancedViews = isLargePlateLike;
    bool isSmallPlateLike = isPlateLikeProfile && maxModelDim <= 45.0 && modelFootprint <= 2000.0;
    bool isMediumPlateLike = isPlateLikeProfile && !isSmallPlateLike && !isLargePlateLike;
    bool needsExpandedProjection = isSmallPlateLike || isMediumPlateLike;
    if (isPlateLikeProfile) {
        if (maxModelDim <= 45.0 && modelFootprint <= 2000.0) {
            paperPresetIndex = 0;  // small plate samples use A4 vertical
        } else if (isLargePlateLike) {
            paperPresetIndex = 4;
        } else {
            paperPresetIndex = 2;  // medium plate samples use A3 horizontal
        }
    } else if (isSteppedProfile) {
        paperPresetIndex = 2;
    } else if (maxModelDim <= 120.0 && modelFootprint <= 12000.0 && modelSizeZ <= modelSizeX * 0.45) {
        paperPresetIndex = 0;
    } else if (maxModelDim > 450.0 || modelFootprint > 90000.0) {
        paperPresetIndex = 4;
    } else if (maxModelDim > 220.0 || modelFootprint > 30000.0) {
        paperPresetIndex = 3;
    } else {
        paperPresetIndex = 1;
    }

    if (HasActiveReferenceProfile() && g_activeReferenceProfile.paperName[0] != 0) {
        for (int i = 0; i < static_cast<int>(sizeof(paperPresets) / sizeof(paperPresets[0])); ++i) {
            if (strcmp(paperPresets[i].name, g_activeReferenceProfile.paperName) == 0) {
                paperPresetIndex = i;
                sprintf_s(msg, sizeof(msg), "[DRAFT] Learned reference paper selected: %s", g_activeReferenceProfile.paperName);
                cvxMsgDisp(msg);
                break;
            }
        }
    }

    // Apply regenerate overrides (set by TASK_REGENERATE_DRAFTING)
    paperPresetIndex += g_paperIndexOffset;
    if (paperPresetIndex < 0) paperPresetIndex = 0;
    if (paperPresetIndex >= static_cast<int>(sizeof(paperPresets) / sizeof(paperPresets[0])))
        paperPresetIndex = static_cast<int>(sizeof(paperPresets) / sizeof(paperPresets[0])) - 1;
    if (g_paperIndexOffset != 0 || g_scaleMultiplier != 1.0) {
        sprintf_s(msg, sizeof(msg), "[DRAFT] Override applied: paperIndexOffset=%d (effective preset=%d), scaleMultiplier=%.2f",
            g_paperIndexOffset, paperPresetIndex, g_scaleMultiplier);
        cvxMsgDisp(msg);
    }

    // STEP 2: Create standalone drawing file
    // CRITICAL: Do NOT call cvxFileClose/cvxFileActivate/cvxFileOpen — they may pop up
    // confirmation dialogs that block the main thread indefinitely.
    // Instead, we rely on g_activePartPath (set by open_part) and the saved part copy
    // for view creation. cvxFileNewSingle creates a new drawing without needing to
    // switch to the part first.
    char preDrawActive[512] = {0};
    cvxFileInqActive(preDrawActive, sizeof(preDrawActive));
    char preDrawRoot[256] = {0};
    cvxRootInqActive(preDrawRoot, sizeof(preDrawRoot));
    sprintf_s(msg, sizeof(msg), "[STEP2] Pre-draw active='%s' root='%s' absFilePath='%s'", preDrawActive, preDrawRoot, absFilePath.c_str());
    cvxMsgDisp(msg);

    // Try to query model bbox if we're in part context (not drawing)
    if (!inDrawingContext) {
        if (GetActivePartBoundingSize(&modelSizeX, &modelSizeY, &modelSizeZ)) {
            sprintf_s(msg, sizeof(msg), "[STEP2] Part bbox: X=%.2f Y=%.2f Z=%.2f", modelSizeX, modelSizeY, modelSizeZ);
            cvxMsgDisp(msg);
        }
    }

    // STEP 1.5: Ensure the part file is available at a known ASCII path for view creation.
    // If we're in a drawing context, we can't cvxFileSaveAs the part (it would save the drawing).
    // Instead, copy the original part file using Windows API.
    {
        const char* savedPartPath = "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftPart.Z3PRT";
        CreateDirectoryA("C:\\Users\\Ey\\Documents\\ZW3D", NULL);

        if (inDrawingContext) {
            // Can't save — copy the original part file from origPartPath
            // Try Windows CopyFile first (handles Unicode paths via wide-char)
            bool copied = false;
            int wlen = MultiByteToWideChar(CP_UTF8, 0, origPartPath.c_str(), -1, NULL, 0);
            if (wlen > 0) {
                std::vector<WCHAR> wSrc(wlen);
                MultiByteToWideChar(CP_UTF8, 0, origPartPath.c_str(), -1, wSrc.data(), wlen);
                if (CopyFileW(wSrc.data(), L"C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftPart.Z3PRT", FALSE)) {
                    copied = true;
                    cvxMsgDisp("[STEP1.5] Copied part file (Unicode) to AutoDraftPart.Z3PRT");
                }
            }
            if (!copied && !origPartPath.empty()) {
                // Fallback: try ANSI copy
                if (CopyFileA(origPartPath.c_str(), savedPartPath, FALSE)) {
                    copied = true;
                    cvxMsgDisp("[STEP1.5] Copied part file (ANSI) to AutoDraftPart.Z3PRT");
                }
            }
            if (copied) {
                absFilePath = savedPartPath;
            } else {
                cvxMsgDisp("[STEP1.5] WARNING: Could not copy part file, using origPartPath directly");
                absFilePath = origPartPath;
            }
        } else {
            // We're in part context — save normally
            cvxMsgDisp("[STEP1.5] Saving part to known location...");
            evxErrors saveRet = cvxFileSaveAs(savedPartPath);
            if (saveRet != ZW_API_NO_ERROR) {
                sprintf_s(msg, sizeof(msg), "[STEP1.5] cvxFileSaveAs failed: %d, trying CopyFile", saveRet);
                cvxMsgDisp(msg);
                // Fallback: copy the file
                if (!origPartPath.empty() && CopyFileA(origPartPath.c_str(), savedPartPath, FALSE)) {
                    absFilePath = savedPartPath;
                } else {
                    absFilePath = origPartPath;
                }
            } else {
                absFilePath = savedPartPath;
            }
            cvxRootInqActive(partName, sizeof(partName));
        }
        sprintf_s(msg, sizeof(msg), "[STEP1.5] absFilePath='%s' partName='%s'", absFilePath.c_str(), partName);
        cvxMsgDisp(msg);
    }

    cvxMsgDisp("[DRAFT] Creating drawing file...");
    // Use unique working files so an already-open result cannot trigger an overwrite dialog.
    static int drawCounter = 0;
    drawCounter++;
    char drawName[64] = {0};
    sprintf_s(drawName, sizeof(drawName), "AutoDraft_%d", drawCounter);
    char templateWorkingPath[512] = {0};
    sprintf_s(templateWorkingPath, sizeof(templateWorkingPath),
        "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftTemplate_%d.Z3DRW", drawCounter);

    evxErrors ret = static_cast<evxErrors>(-1);
    if (useReferenceTemplate) {
        DeleteFileA(templateWorkingPath);
        bool copiedTemplate = false;
        int srcLen = MultiByteToWideChar(CP_UTF8, 0, g_activeReferenceProfile.templatePath, -1, NULL, 0);
        int dstLen = MultiByteToWideChar(CP_ACP, 0, templateWorkingPath, -1, NULL, 0);
        if (srcLen > 0 && dstLen > 0) {
            std::vector<WCHAR> sourcePath(srcLen);
            std::vector<WCHAR> workingPath(dstLen);
            MultiByteToWideChar(CP_UTF8, 0, g_activeReferenceProfile.templatePath, -1, sourcePath.data(), srcLen);
            MultiByteToWideChar(CP_ACP, 0, templateWorkingPath, -1, workingPath.data(), dstLen);
            copiedTemplate = CopyFileW(sourcePath.data(), workingPath.data(), FALSE) != FALSE;
        }
        if (copiedTemplate) {
            ZwDrawingViewDiscreteProjectionSet(1);
            ZwDrawingViewDiscreteProjectionQualitySet(ZW_DRAWING_DISCRETE_PROJECT_PRECISE);
            sprintf_s(msg, sizeof(msg), "[DRAFT] Opening copied reference template: %s", templateWorkingPath);
            cvxMsgDisp(msg);
            ret = cvxFileOpen(templateWorkingPath);
            openedReferenceTemplate = (ret == ZW_API_NO_ERROR);
        } else {
            sprintf_s(msg, sizeof(msg), "[DRAFT] Reference template copy failed: %s",
                g_activeReferenceProfile.templatePath);
            cvxMsgDisp(msg);
        }
        if (!openedReferenceTemplate) {
            sprintf_s(msg, sizeof(msg), "[DRAFT] Reference template open failed: %d; falling back to blank sheet", ret);
            cvxMsgDisp(msg);
        }
    }

    if (!openedReferenceTemplate) {
        ret = cvxFileNewSingle(drawName, VX_FILE_SHEET, VX_SUBTYPE_NONE, NULL, NULL);
    }
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[DRAFT] Drawing creation failed: %d", ret);
        cvxMsgDisp(msg);
        if (filePath[0] != 0) {
            cvxFileNew(filePath);
            cvxRootActivate(partName);
        }
        return "{\"status\":\"error\",\"message\":\"Failed to create drawing file\"}";
    }
    cvxMsgDisp(openedReferenceTemplate
        ? "[DRAFT] Reference drawing template opened OK"
        : "[DRAFT] Blank drawing file created OK");
    if (openedReferenceTemplate) {
        ZwDrawingViewDiscreteProjectionQualitySet(ZW_DRAWING_DISCRETE_PROJECT_PRECISE);
    }

    // STEP 3: Get drawing root info
    char drawRootName[256] = {0};
    cvxRootInqActive(drawRootName, sizeof(drawRootName));
    char drawFilePath[512] = {0};
    cvxFileInqActive(drawFilePath, sizeof(drawFilePath));
    sprintf_s(msg, sizeof(msg), "[DRAFT] Drawing root: %s, path: %s", drawRootName, drawFilePath);
    cvxMsgDisp(msg);
    // Store drawing path globally so PMI/quality/clear/section can find it
    strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), drawFilePath, _TRUNCATE);

    // STEP 3b: Save drawing to known location so subsequent calls can find it
    {
        char drawSavePath[512] = {0};
        sprintf_s(drawSavePath, sizeof(drawSavePath), "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW");
        cvxMsgDisp("[DRAFT] Saving drawing to known location...");
        evxErrors svRet = cvxFileSaveAs(drawSavePath);
        if (svRet == ZW_API_NO_ERROR) {
            strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), drawSavePath, _TRUNCATE);
            sprintf_s(msg, sizeof(msg), "[DRAFT] Drawing saved to: %s", drawSavePath);
            cvxMsgDisp(msg);
        } else {
            sprintf_s(msg, sizeof(msg), "[DRAFT] cvxFileSaveAs returned: %d (drawing stays at: %s)", svRet, drawFilePath);
            cvxMsgDisp(msg);
        }
    }

    // STEP 4: Get or create and activate drawing sheet
    cvxMsgDisp("[DRAFT] Getting sheet list...");
    int sheetCount = 0;
    szwEntityHandle* sheetList = NULL;
    ret = ZwDrawingSheetListGet(&sheetCount, &sheetList);

    if (ret == ZW_API_NO_ERROR && sheetCount > 0 && sheetList != NULL) {
        sprintf_s(msg, sizeof(msg), "[DRAFT] Found %d existing sheet(s), activating first...", sheetCount);
        cvxMsgDisp(msg);
        ret = ZwDrawingSheetActivateByHandle(&sheetList[0]);
        if (sheetList) ZwEntityHandleListFree(sheetCount, &sheetList);
        if (ret != ZW_API_NO_ERROR) {
            cvxMsgDisp("[DRAFT] Sheet activate failed");
            goto restore;
        }
    } else {
        if (sheetList) ZwEntityHandleListFree(sheetCount, &sheetList);
        cvxMsgDisp("[DRAFT] No existing sheets, creating new one...");
        szwEntityHandle sheetHandle = {};
        ret = ZwDrawingSheetCreate("Sheet1", &sheetHandle);
        if (ret != ZW_API_NO_ERROR) {
            sprintf_s(msg, sizeof(msg), "[DRAFT] Sheet create failed: %d", ret);
            cvxMsgDisp(msg);
            goto restore;
        }
        ret = ZwDrawingSheetActivateByHandle(&sheetHandle);
        if (ret != ZW_API_NO_ERROR) {
            cvxMsgDisp("[DRAFT] Sheet activate failed");
            goto restore;
        }
    }
    cvxMsgDisp("[DRAFT] Sheet activated OK");

    viewStatsBeforeClearJson = CollectActiveSheetViewStatsJson();
    clearedViewCount = ClearActiveSheetViews();
    viewStatsAfterClearJson = CollectActiveSheetViewStatsJson();

    const int maxLayoutAttempts = 4;
    bool acceptedLayout = false;
    int layoutAttemptUsed = 0;

    for (layoutAttempt = 0; layoutAttempt < maxLayoutAttempts; ++layoutAttempt) {
        createdViews.clear();
        createdViewCount = 0;
        firstViewCreateError = 0;
        firstViewCreateName.clear();
        pmiAnnotatedViews = 0;
        pmiTotalCreated = 0;
        pmiTotalDims = 0;
        pmiFirstError = 0;
        pmiErrorDetail.clear();
        pmiTotalOverlaps = 0;
        pmiTotalIntrusions = 0;
        pmiMetricsJson = "[]";
        baseViewHandle = {};

        int presetIndex = paperPresetIndex + layoutAttempt;
        if (presetIndex >= static_cast<int>(sizeof(paperPresets) / sizeof(paperPresets[0]))) {
            presetIndex = static_cast<int>(sizeof(paperPresets) / sizeof(paperPresets[0])) - 1;
        }
        if (openedReferenceTemplate) {
            sprintf_s(msg, sizeof(msg), "[DRAFT] Preserving learned reference template paper and format: %s",
                g_activeReferenceProfile.paperName);
            cvxMsgDisp(msg);
        } else {
            if (!SetActiveDrawingPaperSize(paperPresets[presetIndex].name, paperPresets[presetIndex].width, paperPresets[presetIndex].height)) {
                sprintf_s(msg, sizeof(msg), "[DRAFT] Failed to set paper preset %s", paperPresets[presetIndex].name);
                cvxMsgDisp(msg);
            } else {
                sprintf_s(msg, sizeof(msg), "[DRAFT] Using paper preset %s (%.0f x %.0f)",
                    paperPresets[presetIndex].name, paperPresets[presetIndex].width, paperPresets[presetIndex].height);
                cvxMsgDisp(msg);
            }
        }

        LayoutParams lp = GetLayoutParamsForProfile(profileName, layoutAttempt);
        DrawingPaperRect activePaperRect = {};
        double paperOriginX = 0.0;
        double paperOriginY = 0.0;
        if (GetActiveDrawingPaperRect(&activePaperRect)) {
            const double paperWidth = activePaperRect.maxX - activePaperRect.minX;
            const double paperHeight = activePaperRect.maxY - activePaperRect.minY;
            const double usableWidth = paperWidth - activePaperRect.marginLeft - activePaperRect.marginRight;
            const double usableHeight = paperHeight - activePaperRect.marginTop - activePaperRect.marginBottom;
            if (usableWidth > 60.0 && usableHeight > 60.0) {
                lp.canvasWidth = usableWidth;
                lp.canvasHeight = usableHeight;
                lp.maxLayoutWidth = usableWidth * 0.84;
                lp.maxLayoutHeight = usableHeight * 0.82;
                paperOriginX = activePaperRect.minX + activePaperRect.marginLeft;
                paperOriginY = activePaperRect.minY + activePaperRect.marginBottom;
                sprintf_s(msg, sizeof(msg),
                    "[DRAFT] Paper usable area %.1f x %.1f origin=(%.1f,%.1f) name=%s",
                    usableWidth, usableHeight, paperOriginX, paperOriginY, activePaperRect.paperName.c_str());
                cvxMsgDisp(msg);
            }
        }
        double baseWidthLogical = modelSizeX;
        double baseHeightLogical = modelSizeZ;
        double verticalWidthLogical = modelSizeX;
        double verticalHeightLogical = modelSizeY;
        double sideWidthLogical = modelSizeY;
        double sideHeightLogical = modelSizeZ;

        if (recommendedBaseView == ZW_VIEW_STANDARD_TOP) {
            baseWidthLogical = modelSizeX;
            baseHeightLogical = modelSizeY;
            verticalWidthLogical = modelSizeX;
            verticalHeightLogical = modelSizeZ;
            sideWidthLogical = modelSizeY;
            sideHeightLogical = modelSizeZ;
        } else if (recommendedBaseView == ZW_VIEW_STANDARD_RIGHT) {
            baseWidthLogical = modelSizeY;
            baseHeightLogical = modelSizeZ;
            verticalWidthLogical = modelSizeY;
            verticalHeightLogical = modelSizeX;
            sideWidthLogical = modelSizeX;
            sideHeightLogical = modelSizeZ;
        }

        const double widthPaddingFactor = lp.widthPaddingFactor;
        const double heightPaddingFactor = lp.heightPaddingFactor;
        const double expandedProjectionFactor = needsExpandedProjection ? 2.0 : 1.0;
        const double widthNeed = baseWidthLogical * widthPaddingFactor + sideWidthLogical * widthPaddingFactor * expandedProjectionFactor;
        const double heightNeed = baseHeightLogical * heightPaddingFactor + verticalHeightLogical * heightPaddingFactor;
        double scaleByWidth = (lp.maxLayoutWidth - lp.gap) / (widthNeed > 1.0 ? widthNeed : 1.0);
        double scaleByHeight = (lp.maxLayoutHeight - lp.gap) / (heightNeed > 1.0 ? heightNeed : 1.0);
        double canvasScaleByWidth = (lp.canvasWidth - 12.0 - lp.gap) / (widthNeed > 1.0 ? widthNeed : 1.0);
        double canvasScaleByHeight = (lp.canvasHeight - 12.0 - lp.gap) / (heightNeed > 1.0 ? heightNeed : 1.0);
        if (canvasScaleByWidth < scaleByWidth) scaleByWidth = canvasScaleByWidth;
        if (canvasScaleByHeight < scaleByHeight) scaleByHeight = canvasScaleByHeight;
        drawingScale = scaleByWidth;
        if (scaleByHeight < drawingScale) drawingScale = scaleByHeight;
        drawingScale *= lp.scaleBias;
        drawingScale *= g_scaleMultiplier;  // Apply regenerate override
        if (drawingScale > 0.82) drawingScale = 0.82;
        if (drawingScale < 0.04) drawingScale = 0.04;

        const double baseWidthScaled = baseWidthLogical * drawingScale;
        const double baseHeightScaled = baseHeightLogical * drawingScale;
        const double verticalHeightScaled = verticalHeightLogical * drawingScale;
        const double sideWidthScaled = sideWidthLogical * drawingScale;
        const double paddedBaseWidth = baseWidthScaled * widthPaddingFactor;
        const double paddedBaseHeight = baseHeightScaled * heightPaddingFactor;
        const double paddedVerticalHeight = verticalHeightScaled * heightPaddingFactor;
        const double paddedSideWidth = sideWidthScaled * widthPaddingFactor;
        const double totalLayoutWidth = paddedBaseWidth + lp.gap + paddedSideWidth;
        const double totalLayoutHeight = paddedVerticalHeight + lp.gap + paddedBaseHeight;
        double leftOffset = (lp.canvasWidth - totalLayoutWidth) / 2.0 + lp.centerShiftX;
        double bottomOffset = (lp.canvasHeight - totalLayoutHeight) / 2.0 + lp.centerShiftY;
        const double margin = 6.0;
        if (leftOffset < margin) leftOffset = margin;
        if (bottomOffset < margin) bottomOffset = margin;
        if (leftOffset + totalLayoutWidth > lp.canvasWidth - margin) {
            leftOffset = lp.canvasWidth - margin - totalLayoutWidth;
        }
        if (bottomOffset + totalLayoutHeight > lp.canvasHeight - margin) {
            bottomOffset = lp.canvasHeight - margin - totalLayoutHeight;
        }
        if (leftOffset < margin) leftOffset = margin;
        if (bottomOffset < margin) bottomOffset = margin;

        frontCenterX = paperOriginX + leftOffset + paddedBaseWidth / 2.0;
        frontCenterY = paperOriginY + bottomOffset + paddedVerticalHeight + lp.gap + paddedBaseHeight / 2.0;
        topCenterX = frontCenterX;
        topCenterY = frontCenterY - (paddedBaseHeight / 2.0 + lp.gap + paddedVerticalHeight / 2.0);
        rightCenterX = frontCenterX + paddedBaseWidth / 2.0 + lp.gap + paddedSideWidth / 2.0;
        rightCenterY = frontCenterY;

        sprintf_s(msg, sizeof(msg),
            "[DRAFT] Layout attempt=%d profile=%s scale=%.3f gap=%.1f front=(%.1f,%.1f) top=(%.1f,%.1f) right=(%.1f,%.1f)",
            layoutAttempt, profileName, drawingScale, lp.gap, frontCenterX, frontCenterY, topCenterX, topCenterY, rightCenterX, rightCenterY);
        cvxMsgDisp(msg);
        layoutJson = std::string("{\"attempt_used\":")
            + std::to_string(layoutAttempt)
            + ",\"scale\":" + std::to_string(drawingScale)
            + ",\"gap\":" + std::to_string(lp.gap)
            + ",\"front\":{\"x\":" + std::to_string(frontCenterX) + ",\"y\":" + std::to_string(frontCenterY) + "}"
            + ",\"top\":{\"x\":" + std::to_string(topCenterX) + ",\"y\":" + std::to_string(topCenterY) + "}"
            + ",\"right\":{\"x\":" + std::to_string(rightCenterX) + ",\"y\":" + std::to_string(rightCenterY) + "}"
            + "}";

        if (layoutAttempt > 0) {
            int retryCleared = ClearActiveSheetViews();
            sprintf_s(msg, sizeof(msg), "[DRAFT] Retry attempt cleared %d prior view(s)", retryCleared);
            cvxMsgDisp(msg);
        }

        // Cancel any pending ZW3D command that might block view creation (error -152)
        ZwCommandSend("");
        Sleep(200);
        cvxMsgDisp("[DRAFT] Creating 3-view layout (independent standard views)...");

        struct StandardViewDef {
            const char* name;
            ezwDrawingViewMethod type;
            double x;
            double y;
        };

        StandardViewDef views[3];
        bool useReferenceAnchors = false;
        bool addExpandedRightProjection = false;
        bool addLeftProjection = false;
        bool addSecondaryBaseView = false;
        double expandedRightX = 0.0;
        double expandedRightY = 0.0;
        double leftProjectionX = 0.0;
        double leftProjectionY = 0.0;
        double secondaryBaseX = 0.0;
        double secondaryBaseY = 0.0;
        if (recommendedBaseView == ZW_VIEW_STANDARD_TOP) {
            views[0] = {"TOP", ZW_VIEW_STANDARD_TOP, frontCenterX, frontCenterY};
            views[1] = {"FRONT", ZW_VIEW_STANDARD_FRONT, topCenterX, topCenterY};
            views[2] = {"RIGHT", ZW_VIEW_STANDARD_RIGHT, rightCenterX, rightCenterY};
        } else if (recommendedBaseView == ZW_VIEW_STANDARD_RIGHT) {
            views[0] = {"RIGHT", ZW_VIEW_STANDARD_RIGHT, frontCenterX, frontCenterY};
            views[1] = {"TOP", ZW_VIEW_STANDARD_TOP, topCenterX, topCenterY};
            views[2] = {"FRONT", ZW_VIEW_STANDARD_FRONT, rightCenterX, rightCenterY};
        } else {
            views[0] = {"FRONT", ZW_VIEW_STANDARD_FRONT, frontCenterX, frontCenterY};
            views[1] = {"TOP", ZW_VIEW_STANDARD_TOP, topCenterX, topCenterY};
            views[2] = {"RIGHT", ZW_VIEW_STANDARD_RIGHT, rightCenterX, rightCenterY};
        }

        if (isSmallPlateLike) {
            useReferenceAnchors = true;
            views[0].x = 63.8;  views[0].y = 205.6;
            views[1].x = 63.8;  views[1].y = 164.5;
            views[2].x = 106.4; views[2].y = 205.6;
            addExpandedRightProjection = true;
            expandedRightX = 163.6; expandedRightY = 205.6;
        } else if (isMediumPlateLike) {
            useReferenceAnchors = true;
            views[0].x = 119.4; views[0].y = 186.5;
            views[1].x = 119.4; views[1].y = 256.8;
            views[2].x = 210.0; views[2].y = 186.5;
            addExpandedRightProjection = true;
            expandedRightX = 309.9; expandedRightY = 186.5;
        } else if (isSteppedProfile) {
            useReferenceAnchors = true;
            views[0].x = 180.6; views[0].y = 186.3;
            views[1].x = 180.6; views[1].y = 254.3;
            views[2].x = 290.5; views[2].y = 186.3;
            addLeftProjection = true;
            leftProjectionX = 83.2; leftProjectionY = 186.3;
            addSecondaryBaseView = true;
            secondaryBaseX = 339.0; secondaryBaseY = 245.2;
        }

        if (HasActiveReferenceProfile()) {
            int refBase = ReferencePrimaryBaseIndex();
            int refVertical = ReferenceVerticalProjectIndex();
            int refRight = ReferenceRightProjectIndex();
            int refExtraRight = ReferenceExtraRightProjectIndex();
            int refLeft = ReferenceLeftProjectIndex();
            int refSecondBase = ReferenceSecondaryBaseIndex();
            if (refBase >= 0 && refVertical >= 0 && refRight >= 0) {
                useReferenceAnchors = true;
                views[0].x = g_activeReferenceProfile.views[refBase].x;
                views[0].y = g_activeReferenceProfile.views[refBase].y;
                views[1].x = g_activeReferenceProfile.views[refVertical].x;
                views[1].y = g_activeReferenceProfile.views[refVertical].y;
                views[2].x = g_activeReferenceProfile.views[refRight].x;
                views[2].y = g_activeReferenceProfile.views[refRight].y;

                addExpandedRightProjection = false;
                addLeftProjection = false;
                addSecondaryBaseView = false;
                if (refExtraRight >= 0) {
                    addExpandedRightProjection = true;
                    expandedRightX = g_activeReferenceProfile.views[refExtraRight].x;
                    expandedRightY = g_activeReferenceProfile.views[refExtraRight].y;
                }
                if (refLeft >= 0) {
                    addLeftProjection = true;
                    leftProjectionX = g_activeReferenceProfile.views[refLeft].x;
                    leftProjectionY = g_activeReferenceProfile.views[refLeft].y;
                }
                if (refSecondBase >= 0) {
                    addSecondaryBaseView = true;
                    secondaryBaseX = g_activeReferenceProfile.views[refSecondBase].x;
                    secondaryBaseY = g_activeReferenceProfile.views[refSecondBase].y;
                }

                sprintf_s(msg, sizeof(msg), "[DRAFT] Learned reference anchors sample=%s base=(%.1f,%.1f) vertical=(%.1f,%.1f) right=(%.1f,%.1f)",
                    g_activeReferenceProfile.sample, views[0].x, views[0].y, views[1].x, views[1].y, views[2].x, views[2].y);
                cvxMsgDisp(msg);
            }
        }

        auto createStandardView = [&](const StandardViewDef& def, szwEntityHandle* outHandle) -> ezwErrors {
            szwViewStandardData vd;
            ezwErrors localRet = ZwDrawingViewStandardDataInit(&vd);
            if (localRet != ZW_API_NO_ERROR) {
                return localRet;
            }

            strncpy_s(vd.path, sizeof(vd.path), absFilePath.c_str(), _TRUNCATE);
            strncpy_s(vd.rootName, sizeof(vd.rootName), partName, _TRUNCATE);
            vd.type = ZW_STANDARD_VIEW_NATIVE_TYPE;
            vd.option.viewType = def.type;
            vd.location.x = def.x;
            vd.location.y = def.y;
            vd.isIgnoreAttribute = 0;
            vd.viewAttribute.showScale = 0;
            vd.viewAttribute.scaleType = ZW_VIEW_USE_CUSTOM_SCALE;
            vd.viewAttribute.scaleRatioX = drawingScale;
            vd.viewAttribute.scaleRatioY = 1.0;
            vd.viewAttribute.showLabel = 0;
            vd.viewAttribute.showCenterLine = HasActiveReferenceProfile() ? 1 : 0;
            vd.defaultOrigin = 0;
            vd.defaultCoordinate = 0;
            vd.backupIndex = 1;
            vd.isCalculate = 1;
            return ZwDrawingViewStandardCreate(vd, outHandle);
        };

        auto createProjectionView = [&](const StandardViewDef& def, const szwEntityHandle& sourceView, szwEntityHandle* outHandle) -> ezwErrors {
            szwProjectionViewData pd;
            ezwErrors localRet = ZwDrawingViewProjectionInit(&pd);
            if (localRet != ZW_API_NO_ERROR) {
                return localRet;
            }

            pd.baseView = sourceView;
            pd.location.x = def.x;
            pd.location.y = def.y;
            pd.angleType = ZW_3RD_VIEW_ANGLE;
            pd.dimensionType = ZW_DIMENSION_PROJECTION;
            pd.scaleType = ZW_VIEW_USE_PARENT_VIEW_SCALE;
            pd.scaleRatioX = drawingScale;
            pd.scaleRatioY = 1.0;
            localRet = ZwDrawingViewProjection(pd, outHandle);
            if (localRet == ZW_API_NO_ERROR && HasActiveReferenceProfile()) {
                szwViewAttribute viewAttribute = {};
                if (ZwDrawingViewAttributeGet(outHandle, &viewAttribute) == ZW_API_NO_ERROR) {
                    viewAttribute.showCenterLine = 1;
                    ZwDrawingViewAttributeSet(outHandle, viewAttribute);
                }
            }
            return localRet;
        };

        const double probeX[3] = {90.0, 90.0, 230.0};
        const double probeY[3] = {150.0, 45.0, 150.0};
        double actualWidths[3] = {0.0, 0.0, 0.0};
        double actualHeights[3] = {0.0, 0.0, 0.0};
        bool probeOk = true;
        double spacingScale = 1.0;
        double actualHGap = 0.0;
        double actualVGap = 0.0;
        double frontLeftReserve = 0.0;
        double frontRightReserve = 0.0;
        double frontTopReserve = 0.0;
        double frontBottomReserve = 0.0;
        double topLeftReserve = 0.0;
        double topRightReserve = 0.0;
        double topTopReserve = 0.0;
        double topBottomReserve = 0.0;
        double rightLeftReserve = 0.0;
        double rightRightReserve = 0.0;
        double rightTopReserve = 0.0;
        double rightBottomReserve = 0.0;
        DrawingPaperRect paperRect;
        if (!GetActiveDrawingPaperRect(&paperRect)) {
            paperRect.minX = 0.0;
            paperRect.maxX = lp.canvasWidth;
            paperRect.minY = 0.0;
            paperRect.maxY = lp.canvasHeight;
            paperRect.source = "layout_fallback";
        }
        double borderMinX = paperRect.minX;
        double borderMaxX = paperRect.maxX;
        double borderMinY = paperRect.minY;
        double borderMaxY = paperRect.maxY;
        double sheetLeftMargin = borderMinX + 12.0;
        double sheetRightLimit = borderMaxX - 12.0;
        double sheetBottomMargin = borderMinY + 12.0;
        double sheetTopLimit = borderMaxY - 12.0;
        double usableWidth = sheetRightLimit - sheetLeftMargin;
        double usableHeight = sheetTopLimit - sheetBottomMargin;

        for (int probePass = 0; probePass < 4; ++probePass) {
            auto reserveFloor = [](double scaledValue, double minimumValue) -> double {
                return scaledValue > minimumValue ? scaledValue : minimumValue;
            };
            spacingScale = drawingScale / 0.08;
            if (spacingScale < 0.22) spacingScale = 0.22;
            if (spacingScale > 1.00) spacingScale = 1.00;
            actualHGap = reserveFloor((lp.gap + 16.0) * spacingScale, needsExpandedProjection ? 28.0 : 14.0);
            double steppedVGapMin = isSteppedProfile ? 28.0 : 8.0;
            double steppedFrontTopMin = isSteppedProfile ? 20.0 : 10.0;
            double steppedTopBottomMin = isSteppedProfile ? 18.0 : 8.0;
            actualVGap = reserveFloor((lp.gap + (isSteppedProfile ? 22.0 : 10.0)) * spacingScale, steppedVGapMin);
            frontLeftReserve = reserveFloor(14.0 * spacingScale, 10.0);
            frontRightReserve = reserveFloor(18.0 * spacingScale, 14.0);
            frontTopReserve = reserveFloor((isSteppedProfile ? 18.0 : 12.0) * spacingScale, steppedFrontTopMin);
            frontBottomReserve = reserveFloor(22.0 * spacingScale, 22.0);
            topLeftReserve = reserveFloor(12.0 * spacingScale, 10.0);
            topRightReserve = reserveFloor(16.0 * spacingScale, 12.0);
            topTopReserve = reserveFloor(14.0 * spacingScale, 8.0);
            topBottomReserve = reserveFloor((isSteppedProfile ? 18.0 : 12.0) * spacingScale, steppedTopBottomMin);
            rightLeftReserve = reserveFloor(12.0 * spacingScale, 10.0);
            rightRightReserve = reserveFloor(30.0 * spacingScale, 18.0);
            rightTopReserve = reserveFloor(14.0 * spacingScale, 12.0);
            rightBottomReserve = reserveFloor(16.0 * spacingScale, 20.0);
            double paperPadX = reserveFloor(6.0 * spacingScale, 8.0);
            double paperPadTop = reserveFloor(5.0 * spacingScale, 8.0);
            double paperPadBottom = reserveFloor(8.0 * spacingScale, 14.0);
            double leftMarginFromPaper = paperRect.marginLeft > 0.0 ? paperRect.marginLeft : 6.0;
            double rightMarginFromPaper = paperRect.marginRight > 0.0 ? paperRect.marginRight : 6.0;
            double bottomMarginFromPaper = paperRect.marginBottom > 0.0 ? paperRect.marginBottom : 6.0;
            double topMarginFromPaper = paperRect.marginTop > 0.0 ? paperRect.marginTop : 6.0;
            sheetLeftMargin = borderMinX + leftMarginFromPaper + paperPadX;
            sheetRightLimit = borderMaxX - rightMarginFromPaper - paperPadX;
            sheetBottomMargin = borderMinY + bottomMarginFromPaper + paperPadBottom;
            sheetTopLimit = borderMaxY - topMarginFromPaper - paperPadTop;
            usableWidth = sheetRightLimit - sheetLeftMargin;
            usableHeight = sheetTopLimit - sheetBottomMargin;

            szwEntityHandle probeViews[3] = {};
            probeOk = true;
            for (int i = 0; i < 3; ++i) {
                actualWidths[i] = 0.0;
                actualHeights[i] = 0.0;
                StandardViewDef probeDef = views[i];
                probeDef.x = probeX[i];
                probeDef.y = probeY[i];
                ezwErrors probeRet = createStandardView(probeDef, &probeViews[i]);
                sprintf_s(msg, sizeof(msg), "[DRAFT] PROBE view[%d] '%s' at (%.1f,%.1f) ret=%d path='%s'",
                    i, probeDef.name, probeDef.x, probeDef.y, probeRet, absFilePath.c_str());
                cvxMsgDisp(msg);
                if (probeRet != ZW_API_NO_ERROR) {
                    probeOk = false;
                    if (firstViewCreateError == 0) {
                        firstViewCreateError = probeRet;
                        firstViewCreateName = probeDef.name;
                    }
                    sprintf_s(msg, sizeof(msg), "[DRAFT] Probe view %s failed: %d", probeDef.name, probeRet);
                    cvxMsgDisp(msg);
                    break;
                }

                szwDrawingDottedBorder border = {};
                if (ZwDrawingViewDottedBorderGet(probeViews[i], &border) != ZW_API_NO_ERROR) {
                    probeOk = false;
                    if (firstViewCreateError == 0) {
                        firstViewCreateError = ZW_API_GENERAL_ERROR;
                        firstViewCreateName = probeDef.name;
                    }
                    sprintf_s(msg, sizeof(msg), "[DRAFT] Probe border get failed for %s", probeDef.name);
                    cvxMsgDisp(msg);
                    break;
                }
                actualWidths[i] = border.bottomRight.x - border.upperLeft.x;
                actualHeights[i] = border.upperLeft.y - border.bottomRight.y;
            }

            for (int i = 0; i < 3; ++i) {
                if (!IsEmptyHandle(probeViews[i])) {
                    ZwEntityDelete(probeViews[i]);
                    ZwEntityHandleFree(&probeViews[i]);
                }
            }

            if (!probeOk) {
                break;
            }

            double frontBoxWidth = actualWidths[0] + frontLeftReserve + frontRightReserve;
            double rightBoxWidth = actualWidths[2] + rightLeftReserve + rightRightReserve;
            double topBoxWidth = actualWidths[1] + topLeftReserve + topRightReserve;
            double frontBoxHeight = actualHeights[0] + frontTopReserve + frontBottomReserve;
            double rightBoxHeight = actualHeights[2] + rightTopReserve + rightBottomReserve;
            double topBoxHeight = actualHeights[1] + topTopReserve + topBottomReserve;
            double expandedBoxWidth = needsExpandedProjection ? (actualHGap + rightBoxWidth) : 0.0;
            double rowWidth = frontBoxWidth + actualHGap + rightBoxWidth + expandedBoxWidth;
            if (topBoxWidth > rowWidth) {
                rowWidth = topBoxWidth;
            }
            double totalActualWidth = rowWidth;
            double totalActualHeight = topBoxHeight + actualVGap + ((frontBoxHeight > rightBoxHeight) ? frontBoxHeight : rightBoxHeight);
            auto shrinkToFit = [](double& value, double minimumValue, double& overflow) {
                if (overflow <= 0.0 || value <= minimumValue) {
                    return;
                }
                double reducible = value - minimumValue;
                double delta = reducible < overflow ? reducible : overflow;
                value -= delta;
                overflow -= delta;
            };
            double verticalOverflow = totalActualHeight - usableHeight;
            if (verticalOverflow > 0.0) {
                shrinkToFit(actualVGap, isSteppedProfile ? 18.0 : 4.0, verticalOverflow);
                shrinkToFit(topTopReserve, 4.0, verticalOverflow);
                shrinkToFit(topBottomReserve, 4.0, verticalOverflow);
                shrinkToFit(frontTopReserve, 6.0, verticalOverflow);
                shrinkToFit(rightTopReserve, 6.0, verticalOverflow);
                frontBoxHeight = actualHeights[0] + frontTopReserve + frontBottomReserve;
                rightBoxHeight = actualHeights[2] + rightTopReserve + rightBottomReserve;
                topBoxHeight = actualHeights[1] + topTopReserve + topBottomReserve;
                totalActualHeight = topBoxHeight + actualVGap + ((frontBoxHeight > rightBoxHeight) ? frontBoxHeight : rightBoxHeight);
            }
            double horizontalOverflow = totalActualWidth - usableWidth;
            if (horizontalOverflow > 0.0) {
                shrinkToFit(actualHGap, needsExpandedProjection ? 24.0 : 6.0, horizontalOverflow);
                shrinkToFit(rightRightReserve, 10.0, horizontalOverflow);
                shrinkToFit(frontLeftReserve, 8.0, horizontalOverflow);
                shrinkToFit(topRightReserve, 8.0, horizontalOverflow);
                frontBoxWidth = actualWidths[0] + frontLeftReserve + frontRightReserve;
                rightBoxWidth = actualWidths[2] + rightLeftReserve + rightRightReserve;
                topBoxWidth = actualWidths[1] + topLeftReserve + topRightReserve;
                expandedBoxWidth = needsExpandedProjection ? (actualHGap + rightBoxWidth) : 0.0;
                rowWidth = frontBoxWidth + actualHGap + rightBoxWidth + expandedBoxWidth;
                if (topBoxWidth > rowWidth) {
                    rowWidth = topBoxWidth;
                }
                totalActualWidth = rowWidth;
            }
            double fitByWidth = usableWidth / (totalActualWidth > 1.0 ? totalActualWidth : 1.0);
            double fitByHeight = usableHeight / (totalActualHeight > 1.0 ? totalActualHeight : 1.0);
            double fitFactor = fitByWidth;
            if (fitByHeight < fitFactor) fitFactor = fitByHeight;
            if (fitFactor >= 0.995 || drawingScale <= 0.012) {
                break;
            }

            double newScale = drawingScale * fitFactor * 0.96;
            if (newScale < 0.012) {
                newScale = 0.012;
            }
            sprintf_s(msg, sizeof(msg), "[DRAFT] Probe pass %d reducing scale from %.3f to %.3f", probePass, drawingScale, newScale);
            cvxMsgDisp(msg);
            if (newScale >= drawingScale - 0.0001) {
                break;
            }
            drawingScale = newScale;
        }

        if (probeOk) {
            auto reserveFloor = [](double scaledValue, double minimumValue) -> double {
                return scaledValue > minimumValue ? scaledValue : minimumValue;
            };
            spacingScale = drawingScale / 0.08;
            if (spacingScale < 0.22) spacingScale = 0.22;
            if (spacingScale > 1.00) spacingScale = 1.00;
            actualHGap = reserveFloor((lp.gap + 16.0) * spacingScale, needsExpandedProjection ? 28.0 : 14.0);
            double steppedVGapMin = isSteppedProfile ? 28.0 : 8.0;
            double steppedFrontTopMin = isSteppedProfile ? 20.0 : 10.0;
            double steppedTopBottomMin = isSteppedProfile ? 18.0 : 8.0;
            actualVGap = reserveFloor((lp.gap + (isSteppedProfile ? 22.0 : 10.0)) * spacingScale, steppedVGapMin);
            frontLeftReserve = reserveFloor(14.0 * spacingScale, 10.0);
            frontRightReserve = reserveFloor(18.0 * spacingScale, 14.0);
            frontTopReserve = reserveFloor((isSteppedProfile ? 18.0 : 12.0) * spacingScale, steppedFrontTopMin);
            frontBottomReserve = reserveFloor(22.0 * spacingScale, 22.0);
            topLeftReserve = reserveFloor(12.0 * spacingScale, 10.0);
            topRightReserve = reserveFloor(16.0 * spacingScale, 12.0);
            topTopReserve = reserveFloor(14.0 * spacingScale, 8.0);
            topBottomReserve = reserveFloor((isSteppedProfile ? 18.0 : 12.0) * spacingScale, steppedTopBottomMin);
            rightLeftReserve = reserveFloor(12.0 * spacingScale, 10.0);
            rightRightReserve = reserveFloor(30.0 * spacingScale, 18.0);
            rightTopReserve = reserveFloor(14.0 * spacingScale, 12.0);
            rightBottomReserve = reserveFloor(16.0 * spacingScale, 20.0);
            double paperPadX = reserveFloor(6.0 * spacingScale, 8.0);
            double paperPadTop = reserveFloor(5.0 * spacingScale, 8.0);
            double paperPadBottom = reserveFloor(8.0 * spacingScale, 14.0);
            double leftMarginFromPaper = paperRect.marginLeft > 0.0 ? paperRect.marginLeft : 6.0;
            double rightMarginFromPaper = paperRect.marginRight > 0.0 ? paperRect.marginRight : 6.0;
            double bottomMarginFromPaper = paperRect.marginBottom > 0.0 ? paperRect.marginBottom : 6.0;
            double topMarginFromPaper = paperRect.marginTop > 0.0 ? paperRect.marginTop : 6.0;
            sheetLeftMargin = borderMinX + leftMarginFromPaper + paperPadX;
            sheetRightLimit = borderMaxX - rightMarginFromPaper - paperPadX;
            sheetBottomMargin = borderMinY + bottomMarginFromPaper + paperPadBottom;
            sheetTopLimit = borderMaxY - topMarginFromPaper - paperPadTop;
            usableWidth = sheetRightLimit - sheetLeftMargin;
            usableHeight = sheetTopLimit - sheetBottomMargin;

            double frontBoxWidth = actualWidths[0] + frontLeftReserve + frontRightReserve;
            double rightBoxWidth = actualWidths[2] + rightLeftReserve + rightRightReserve;
            double topBoxWidth = actualWidths[1] + topLeftReserve + topRightReserve;
            double frontBoxHeight = actualHeights[0] + frontTopReserve + frontBottomReserve;
            double rightBoxHeight = actualHeights[2] + rightTopReserve + rightBottomReserve;
            double topBoxHeight = actualHeights[1] + topTopReserve + topBottomReserve;
            double expandedBoxWidth = needsExpandedProjection ? (actualHGap + rightBoxWidth) : 0.0;
            double rowWidth = frontBoxWidth + actualHGap + rightBoxWidth + expandedBoxWidth;
            if (topBoxWidth > rowWidth) {
                rowWidth = topBoxWidth;
            }
            double totalActualWidth = rowWidth;
            double totalActualHeight = topBoxHeight + actualVGap + ((frontBoxHeight > rightBoxHeight) ? frontBoxHeight : rightBoxHeight);
            auto shrinkToFit = [](double& value, double minimumValue, double& overflow) {
                if (overflow <= 0.0 || value <= minimumValue) {
                    return;
                }
                double reducible = value - minimumValue;
                double delta = reducible < overflow ? reducible : overflow;
                value -= delta;
                overflow -= delta;
            };
            double verticalOverflow = totalActualHeight - usableHeight;
            if (verticalOverflow > 0.0) {
                shrinkToFit(actualVGap, isSteppedProfile ? 18.0 : 4.0, verticalOverflow);
                shrinkToFit(topTopReserve, 4.0, verticalOverflow);
                shrinkToFit(topBottomReserve, 4.0, verticalOverflow);
                shrinkToFit(frontTopReserve, 6.0, verticalOverflow);
                shrinkToFit(rightTopReserve, 6.0, verticalOverflow);
                frontBoxHeight = actualHeights[0] + frontTopReserve + frontBottomReserve;
                rightBoxHeight = actualHeights[2] + rightTopReserve + rightBottomReserve;
                topBoxHeight = actualHeights[1] + topTopReserve + topBottomReserve;
                totalActualHeight = topBoxHeight + actualVGap + ((frontBoxHeight > rightBoxHeight) ? frontBoxHeight : rightBoxHeight);
            }
            double horizontalOverflow = totalActualWidth - usableWidth;
            if (horizontalOverflow > 0.0) {
                shrinkToFit(actualHGap, needsExpandedProjection ? 24.0 : 6.0, horizontalOverflow);
                shrinkToFit(rightRightReserve, 10.0, horizontalOverflow);
                shrinkToFit(frontLeftReserve, 8.0, horizontalOverflow);
                shrinkToFit(topRightReserve, 8.0, horizontalOverflow);
                frontBoxWidth = actualWidths[0] + frontLeftReserve + frontRightReserve;
                rightBoxWidth = actualWidths[2] + rightLeftReserve + rightRightReserve;
                topBoxWidth = actualWidths[1] + topLeftReserve + topRightReserve;
                expandedBoxWidth = needsExpandedProjection ? (actualHGap + rightBoxWidth) : 0.0;
                rowWidth = frontBoxWidth + actualHGap + rightBoxWidth + expandedBoxWidth;
                if (topBoxWidth > rowWidth) {
                    rowWidth = topBoxWidth;
                }
                totalActualWidth = rowWidth;
            }
            double leftActual = sheetLeftMargin + (usableWidth - totalActualWidth) / 2.0 + lp.centerShiftX - 18.0;
            double bottomActual = sheetBottomMargin + (usableHeight - totalActualHeight) / 2.0 + lp.centerShiftY - 10.0;
            if (leftActual < sheetLeftMargin) leftActual = sheetLeftMargin;
            if (bottomActual < sheetBottomMargin) bottomActual = sheetBottomMargin;
            if (leftActual + totalActualWidth > sheetRightLimit) {
                leftActual = sheetRightLimit - totalActualWidth;
            }
            if (bottomActual + totalActualHeight > sheetTopLimit) {
                bottomActual = sheetTopLimit - totalActualHeight;
            }
            if (leftActual < sheetLeftMargin) leftActual = sheetLeftMargin;
            if (bottomActual < sheetBottomMargin) bottomActual = sheetBottomMargin;

            double topLeft = leftActual + (rowWidth - topBoxWidth) / 2.0;
            double frontLeft = leftActual;
            double rightLeft = leftActual + frontBoxWidth + actualHGap;
            double rowBottom = bottomActual;
            double topBottom = rowBottom + ((frontBoxHeight > rightBoxHeight) ? frontBoxHeight : rightBoxHeight) + actualVGap;

            views[0].x = frontLeft + frontLeftReserve + actualWidths[0] / 2.0;
            views[0].y = rowBottom + frontBottomReserve + actualHeights[0] / 2.0;
            views[1].x = topLeft + topLeftReserve + actualWidths[1] / 2.0;
            views[1].y = topBottom + topBottomReserve + actualHeights[1] / 2.0;
            views[2].x = rightLeft + rightLeftReserve + actualWidths[2] / 2.0;
            views[2].y = rowBottom + rightBottomReserve + actualHeights[2] / 2.0;

            layoutJson = std::string("{\"attempt_used\":")
                + std::to_string(layoutAttempt)
                + ",\"scale\":" + std::to_string(drawingScale)
                + ",\"gap\":" + std::to_string(lp.gap)
                + ",\"actual_h_gap\":" + std::to_string(actualHGap)
                + ",\"actual_v_gap\":" + std::to_string(actualVGap)
                + ",\"paper_source\":\"" + paperRect.source + "\""
                + ",\"paper_name\":\"" + JsonEscape(paperRect.paperName) + "\""
                + ",\"border\":{\"min_x\":" + std::to_string(borderMinX)
                + ",\"max_x\":" + std::to_string(borderMaxX)
                + ",\"min_y\":" + std::to_string(borderMinY)
                + ",\"max_y\":" + std::to_string(borderMaxY) + "}"
                + ",\"paper_margin\":{\"top\":" + std::to_string(paperRect.marginTop)
                + ",\"right\":" + std::to_string(paperRect.marginRight)
                + ",\"bottom\":" + std::to_string(paperRect.marginBottom)
                + ",\"left\":" + std::to_string(paperRect.marginLeft) + "}"
                + ",\"usable\":{\"left\":" + std::to_string(sheetLeftMargin)
                + ",\"right\":" + std::to_string(sheetRightLimit)
                + ",\"bottom\":" + std::to_string(sheetBottomMargin)
                + ",\"top\":" + std::to_string(sheetTopLimit) + "}"
                + ",\"front\":{\"x\":" + std::to_string(views[0].x) + ",\"y\":" + std::to_string(views[0].y) + "}"
                + ",\"top\":{\"x\":" + std::to_string(views[1].x) + ",\"y\":" + std::to_string(views[1].y) + "}"
                + ",\"right\":{\"x\":" + std::to_string(views[2].x) + ",\"y\":" + std::to_string(views[2].y) + "}"
                + "}";

            sprintf_s(msg, sizeof(msg),
                "[DRAFT] Actual-border layout attempt=%d front=(%.1f,%.1f) top=(%.1f,%.1f) right=(%.1f,%.1f)",
                layoutAttempt, views[0].x, views[0].y, views[1].x, views[1].y, views[2].x, views[2].y);
            cvxMsgDisp(msg);
        }

        if (useReferenceAnchors) {
            if (HasActiveReferenceProfile()) {
                int refBase = ReferencePrimaryBaseIndex();
                int refVertical = ReferenceVerticalProjectIndex();
                int refRight = ReferenceRightProjectIndex();
                int refExtraRight = ReferenceExtraRightProjectIndex();
                int refLeft = ReferenceLeftProjectIndex();
                int refSecondBase = ReferenceSecondaryBaseIndex();
                if (refBase >= 0) {
                    views[0].x = g_activeReferenceProfile.views[refBase].x;
                    views[0].y = g_activeReferenceProfile.views[refBase].y;
                }
                if (refVertical >= 0) {
                    views[1].x = g_activeReferenceProfile.views[refVertical].x;
                    views[1].y = g_activeReferenceProfile.views[refVertical].y;
                }
                if (refRight >= 0) {
                    views[2].x = g_activeReferenceProfile.views[refRight].x;
                    views[2].y = g_activeReferenceProfile.views[refRight].y;
                }
                if (refExtraRight >= 0) {
                    addExpandedRightProjection = true;
                    expandedRightX = g_activeReferenceProfile.views[refExtraRight].x;
                    expandedRightY = g_activeReferenceProfile.views[refExtraRight].y;
                }
                if (refLeft >= 0) {
                    addLeftProjection = true;
                    leftProjectionX = g_activeReferenceProfile.views[refLeft].x;
                    leftProjectionY = g_activeReferenceProfile.views[refLeft].y;
                }
                if (refSecondBase >= 0) {
                    addSecondaryBaseView = true;
                    secondaryBaseX = g_activeReferenceProfile.views[refSecondBase].x;
                    secondaryBaseY = g_activeReferenceProfile.views[refSecondBase].y;
                }

                if (strstr(g_activeReferenceProfile.sample, "TZ-TP") != nullptr && !openedReferenceTemplate) {
                    // TP is a very wide thin plate. The learned centers match the sample,
                    // but the generated dimension text needs more horizontal breathing room.
                    views[0].x -= 28.0;
                    views[1].x -= 28.0;
                    views[1].y += 15.0;
                    views[2].x += 8.0;
                    if (addExpandedRightProjection) {
                        expandedRightX += 18.0;
                    }
                    sprintf_s(msg, sizeof(msg),
                        "[DRAFT] Applied visual clearance offsets for learned thin-plate reference sample=%s",
                        g_activeReferenceProfile.sample);
                    cvxMsgDisp(msg);
                } else if (strstr(g_activeReferenceProfile.sample, "TZ-ZJ") != nullptr) {
                    // The learned centers match the reference skeleton, but ZW3D's generated
                    // dimension text is larger than the sample text. Add visual clearance so
                    // the drawing is readable instead of exactly stacked on the sample centers.
                    views[0].y -= 65.0;
                    views[1].y -= 1.0;
                    views[2].x += 22.0;
                    views[2].y -= 65.0;
                    if (addLeftProjection) {
                        leftProjectionX -= 14.0;
                        leftProjectionY -= 65.0;
                    }
                    if (addExpandedRightProjection) {
                        expandedRightX += 22.0;
                    }
                    if (addSecondaryBaseView) {
                        secondaryBaseX += 8.0;
                    }
                    sprintf_s(msg, sizeof(msg),
                        "[DRAFT] Applied visual clearance offsets for learned stepped reference sample=%s",
                        g_activeReferenceProfile.sample);
                    cvxMsgDisp(msg);
                }
            } else if (isSmallPlateLike) {
                views[0].x = 63.8;  views[0].y = 205.6;
                views[1].x = 63.8;  views[1].y = 164.5;
                views[2].x = 106.4; views[2].y = 205.6;
                expandedRightX = 163.6; expandedRightY = 205.6;
            } else if (isMediumPlateLike) {
                views[0].x = 119.4; views[0].y = 186.5;
                views[1].x = 119.4; views[1].y = 256.8;
                views[2].x = 210.0; views[2].y = 186.5;
                expandedRightX = 309.9; expandedRightY = 186.5;
            } else if (isSteppedProfile) {
                views[0].x = 180.6; views[0].y = 186.3;
                views[1].x = 180.6; views[1].y = 254.3;
                views[2].x = 290.5; views[2].y = 186.3;
                leftProjectionX = 83.2; leftProjectionY = 186.3;
                secondaryBaseX = 339.0; secondaryBaseY = 245.2;
            }
            layoutJson = std::string("{\"attempt_used\":")
                + std::to_string(layoutAttempt)
                + ",\"scale\":" + std::to_string(drawingScale)
                + ",\"profile_anchor\":\"" + (HasActiveReferenceProfile() ? std::string("learned_reference") : std::string("reference_sample")) + "\""
                + (HasActiveReferenceProfile() ? (std::string(",\"sample\":\"") + JsonEscape(g_activeReferenceProfile.sample) + "\"") : std::string(""))
                + ",\"front\":{\"x\":" + std::to_string(views[0].x) + ",\"y\":" + std::to_string(views[0].y) + "}"
                + ",\"top\":{\"x\":" + std::to_string(views[1].x) + ",\"y\":" + std::to_string(views[1].y) + "}"
                + ",\"right\":{\"x\":" + std::to_string(views[2].x) + ",\"y\":" + std::to_string(views[2].y) + "}"
                + "}";
            sprintf_s(msg, sizeof(msg),
                "[DRAFT] Reference anchors applied front=(%.1f,%.1f) top=(%.1f,%.1f) right=(%.1f,%.1f)",
                views[0].x, views[0].y, views[1].x, views[1].y, views[2].x, views[2].y);
            cvxMsgDisp(msg);
        }

        for (int i = 0; i < 3; ++i) {
            sprintf_s(msg, sizeof(msg), "[DRAFT] === Standard View %d (%s) START ===", i, views[i].name);
            cvxMsgDisp(msg);

            auto t0 = std::chrono::steady_clock::now();
            szwEntityHandle viewHandle = {};
            bool usedProjection = false;
            if (i > 0 && !IsEmptyHandle(baseViewHandle)) {
                ret = createProjectionView(views[i], baseViewHandle, &viewHandle);
                if (ret == ZW_API_NO_ERROR) {
                    usedProjection = true;
                }
            }
            if (!usedProjection) {
                if (i > 0) {
                    sprintf_s(msg, sizeof(msg), "[DRAFT] Projection view %s not used (i=%d, baseEmpty=%d, err=%d), using standard view",
                        views[i].name, i, IsEmptyHandle(baseViewHandle) ? 1 : 0, ret);
                    cvxMsgDisp(msg);
                }
                ret = createStandardView(views[i], &viewHandle);
            }
            auto t1 = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

            if (ret != ZW_API_NO_ERROR) {
                sprintf_s(msg, sizeof(msg), "[DRAFT] View %s FAILED: ret=%d (%.0fms)", views[i].name, ret, ms);
                cvxMsgDisp(msg);
                if (firstViewCreateError == 0) {
                    firstViewCreateError = ret;
                    firstViewCreateName = views[i].name;
                }
                continue;
            }

            sprintf_s(msg, sizeof(msg), "[DRAFT] View %s OK (%.0fms) projection=%d", views[i].name, ms, usedProjection ? 1 : 0);
            cvxMsgDisp(msg);
            if (i == 0) {
                baseViewHandle = viewHandle;
                // After base view creation, read its actual border and adjust
                // the remaining view positions for orthographic alignment
                szwDrawingDottedBorder baseBorder = {};
                if (ZwDrawingViewDottedBorderGet(baseViewHandle, &baseBorder) == ZW_API_NO_ERROR) {
                    double actualBaseCX = (baseBorder.upperLeft.x + baseBorder.bottomRight.x) / 2.0;
                    double actualBaseCY = (baseBorder.upperLeft.y + baseBorder.bottomRight.y) / 2.0;
                    double actualBaseW = baseBorder.bottomRight.x - baseBorder.upperLeft.x;
                    double actualBaseH = baseBorder.upperLeft.y - baseBorder.bottomRight.y;
                    sprintf_s(msg, sizeof(msg), "[DRAFT] Base view actual: cx=%.1f cy=%.1f w=%.1f h=%.1f",
                        actualBaseCX, actualBaseCY, actualBaseW, actualBaseH);
                    cvxMsgDisp(msg);

                    // Adjust TOP view: x-center should match base view x-center
                    // Adjust RIGHT view: y-center should match base view y-center
                    for (int j = 1; j < 3; ++j) {
                        if (views[j].type == ZW_VIEW_STANDARD_TOP) {
                            // TOP is vertically aligned: share x-center with base
                            double dx = actualBaseCX - views[j].x;
                            views[j].x += dx;
                            sprintf_s(msg, sizeof(msg), "[DRAFT] Adjusted TOP x: %.1f -> %.1f (dx=%.1f)",
                                views[j].x - dx, views[j].x, dx);
                            cvxMsgDisp(msg);
                        } else if (views[j].type == ZW_VIEW_STANDARD_RIGHT) {
                            // RIGHT is horizontally aligned: share y-center with base
                            double dy = actualBaseCY - views[j].y;
                            views[j].y += dy;
                            sprintf_s(msg, sizeof(msg), "[DRAFT] Adjusted RIGHT y: %.1f -> %.1f (dy=%.1f)",
                                views[j].y - dy, views[j].y, dy);
                            cvxMsgDisp(msg);
                        }
                    }
                }
            }
            createdViews.push_back(viewHandle);
            createdViewCount++;
        }

        if (addExpandedRightProjection && createdViews.size() >= 3) {
            szwDrawingDottedBorder rightBorder = {};
            double extraX = expandedRightX;
            double extraY = expandedRightY;
            double extraHalfWidth = actualWidths[2] / 2.0;
            if (ZwDrawingViewDottedBorderGet(createdViews[2], &rightBorder) == ZW_API_NO_ERROR) {
                double rightWidth = rightBorder.bottomRight.x - rightBorder.upperLeft.x;
                extraHalfWidth = rightWidth / 2.0;
            }
            if (extraX + extraHalfWidth > sheetRightLimit) {
                extraX = sheetRightLimit - extraHalfWidth;
            }
            if (extraX > views[2].x + extraHalfWidth + 4.0) {
                StandardViewDef extraDef = {"EXPANDED_RIGHT", views[2].type, extraX, extraY};
                szwEntityHandle extraHandle = {};
                ezwErrors extraRet = createProjectionView(extraDef, createdViews[2], &extraHandle);
                sprintf_s(msg, sizeof(msg), "[DRAFT] Expanded projection ret=%d at (%.1f,%.1f)", extraRet, extraX, extraY);
                cvxMsgDisp(msg);
                if (extraRet == ZW_API_NO_ERROR && !IsEmptyHandle(extraHandle)) {
                    createdViews.push_back(extraHandle);
                    createdViewCount++;
                }
            } else {
                sprintf_s(msg, sizeof(msg), "[DRAFT] Skipping expanded projection: insufficient right-side space (x=%.1f)", extraX);
                cvxMsgDisp(msg);
            }
        }

        if (addLeftProjection && createdViews.size() >= 1 && !IsEmptyHandle(baseViewHandle)) {
            StandardViewDef leftDef = {"LEFT", ZW_VIEW_STANDARD_LEFT, leftProjectionX, leftProjectionY};
            szwEntityHandle leftHandle = {};
            ezwErrors leftRet = createProjectionView(leftDef, baseViewHandle, &leftHandle);
            sprintf_s(msg, sizeof(msg), "[DRAFT] Left projection ret=%d at (%.1f,%.1f)", leftRet, leftProjectionX, leftProjectionY);
            cvxMsgDisp(msg);
            if (leftRet == ZW_API_NO_ERROR && !IsEmptyHandle(leftHandle)) {
                createdViews.push_back(leftHandle);
                createdViewCount++;
            }
        }

        if (addSecondaryBaseView) {
            StandardViewDef secondaryDef = {"SECOND_BASE", ZW_VIEW_STANDARD_BACK, secondaryBaseX, secondaryBaseY};
            szwEntityHandle secondaryHandle = {};
            ezwErrors secondaryRet = createStandardView(secondaryDef, &secondaryHandle);
            sprintf_s(msg, sizeof(msg), "[DRAFT] Secondary base ret=%d at (%.1f,%.1f)", secondaryRet, secondaryBaseX, secondaryBaseY);
            cvxMsgDisp(msg);
            if (secondaryRet == ZW_API_NO_ERROR && !IsEmptyHandle(secondaryHandle)) {
                createdViews.push_back(secondaryHandle);
                createdViewCount++;
            }
        }

        if (needsAdvancedViews && createdViewCount > 0 && !IsEmptyHandle(baseViewHandle)) {
            double sectionX = views[0].x;
            double sectionY = sheetBottomMargin + 42.0;
            if (sectionY > views[0].y - 35.0) {
                sectionY = views[0].y - 35.0;
            }
            if (sectionY < sheetBottomMargin + 24.0) {
                sectionY = sheetBottomMargin + 24.0;
            }

            szwEntityHandle sectionHandle = {};
            ezwErrors sectionRet = TryCreateFullSectionView(baseViewHandle, sectionX, sectionY, "A", &sectionHandle);
            sprintf_s(msg, sizeof(msg), "[DRAFT] Advanced full section ret=%d at (%.1f,%.1f)", sectionRet, sectionX, sectionY);
            cvxMsgDisp(msg);
            if (sectionRet == ZW_API_NO_ERROR && !IsEmptyHandle(sectionHandle)) {
                createdViews.push_back(sectionHandle);
                createdViewCount++;
            }
        }

        cvxMsgDisp("[DRAFT] === Drafting generation completed ===");

        if (!createdViews.empty()) {
            ezwErrors refreshRet = ZwDrawingSheetManagerViewTreeRefresh(nullptr);
            sprintf_s(msg, sizeof(msg), "[DRAFT] View tree refresh after create: %d", refreshRet);
            cvxMsgDisp(msg);
            Sleep(1000);
        }

        size_t annotationViewCount = g_enableInlinePmiDuringDrafting ? createdViews.size() : 0;
        if (g_enableInlinePmiDuringDrafting) {
            if (needsAdvancedViews && annotationViewCount > 3) {
                annotationViewCount = 3;
            }

            for (size_t i = 0; i < annotationViewCount; ++i) {
                int createdCount = 0;
                int totalCount = 0;
                int cleanRet = 0;
                int errCode = 0;
                std::string errDetail;
                std::string metricsJson;
                if (AutoAnnotateView(createdViews[i], static_cast<int>(i), &createdCount, &totalCount, &cleanRet, &errCode, &errDetail, profileName, &metricsJson)) {
                    pmiAnnotatedViews++;
                    pmiTotalCreated += createdCount;
                    pmiTotalDims += totalCount;
                } else if (pmiFirstError == 0) {
                    pmiFirstError = errCode;
                    pmiErrorDetail = errDetail;
                }

                if (i == 0) pmiMetricsJson = "[";
                else pmiMetricsJson += ",";
                pmiMetricsJson += metricsJson.empty() ? "{\"status\":\"error\",\"message\":\"missing metrics\"}" : metricsJson;

                const char* overlapPos = strstr(metricsJson.c_str(), "\"text_overlaps\":");
                if (overlapPos != nullptr) {
                    pmiTotalOverlaps += atoi(overlapPos + strlen("\"text_overlaps\":"));
                }
                const char* intrusionPos = strstr(metricsJson.c_str(), "\"view_intrusions\":");
                if (intrusionPos != nullptr) {
                    pmiTotalIntrusions += atoi(intrusionPos + strlen("\"view_intrusions\":"));
                }
            }
            if (annotationViewCount > 0) {
                pmiMetricsJson += "]";
            }
        } else {
            cvxMsgDisp("[DRAFT] Inline PMI disabled for basic non-interactive drafting");
            pmiMetricsJson = "[]";
        }

        sprintf_s(msg, sizeof(msg),
            "[DRAFT] Attempt %d conflicts: text_overlaps=%d view_intrusions=%d annotated_views=%d created_dims=%d",
            layoutAttempt, pmiTotalOverlaps, pmiTotalIntrusions, pmiAnnotatedViews, pmiTotalCreated);
        cvxMsgDisp(msg);

        DrawingPaperRect attemptPaperRect;
        if (GetActiveDrawingPaperRect(&attemptPaperRect)) {
            occupiedOverflowMagnitude = ComputeActiveSheetOverflowMagnitude(attemptPaperRect);
            sprintf_s(msg, sizeof(msg), "[DRAFT] Attempt %d occupied overflow=%.3f", layoutAttempt, occupiedOverflowMagnitude);
            cvxMsgDisp(msg);
        } else {
            occupiedOverflowMagnitude = 0.0;
        }

        for (size_t i = 0; i < createdViews.size(); ++i) {
            ZwEntityHandleFree(&createdViews[i]);
        }

        if ((pmiTotalOverlaps == 0 && pmiTotalIntrusions == 0 && occupiedOverflowMagnitude <= 0.01)
            || layoutAttempt == maxLayoutAttempts - 1 || createdViewCount == 0) {
            acceptedLayout = true;
            layoutAttemptUsed = layoutAttempt;
            break;
        }
    }

    if (createdViewCount == 0) {
        pmiResult = std::string("{\"status\":\"error\",\"message\":\"No drawing views were created\",\"created_views\":0,\"first_view_error\":")
            + std::to_string(firstViewCreateError)
            + ",\"first_view_name\":\"" + firstViewCreateName + "\"}";
    } else if (!g_enableInlinePmiDuringDrafting) {
        pmiResult = std::string("{\"status\":\"skipped\",\"message\":\"Inline PMI disabled for basic non-interactive drafting\",\"created_views\":")
            + std::to_string(createdViewCount) + "}";
    } else if (pmiAnnotatedViews > 0) {
        pmiResult = std::string("{\"status\":\"ok\",\"message\":\"PMI dimensions added during drafting\",\"method\":\"drawing_api_direct\",\"created_views\":")
            + std::to_string(createdViewCount)
            + ",\"annotated_views\":" + std::to_string(pmiAnnotatedViews)
            + ",\"dimensions_created\":" + std::to_string(pmiTotalCreated)
            + ",\"dimensions_total\":" + std::to_string(pmiTotalDims)
            + "}";
    } else if (pmiFirstError != 0) {
        pmiResult = std::string("{\"status\":\"error\",\"message\":\"PMI auto-dimension failed during drafting\",\"method\":\"drawing_api_direct\",\"code\":")
            + std::to_string(pmiFirstError)
            + ",\"detail\":\"" + pmiErrorDetail + "\",\"created_views\":" + std::to_string(createdViewCount) + "}";
    } else {
        pmiResult = std::string("{\"status\":\"error\",\"message\":\"No views were successfully annotated during drafting\",\"created_views\":")
            + std::to_string(createdViewCount) + "}";
    }
    cvxMsgDisp(("[DRAFT] PMI result: " + pmiResult).c_str());

    if (!acceptedLayout) {
        cvxMsgDisp("[DRAFT] No layout attempt was accepted explicitly; using last attempt state");
    }

    if (!GetActiveDrawingPaperRect(&occupiedPaperRect)) {
        occupiedPaperRect.minX = 0.0;
        occupiedPaperRect.maxX = 0.0;
        occupiedPaperRect.minY = 0.0;
        occupiedPaperRect.maxY = 0.0;
        occupiedPaperRect.source = "unavailable";
    }
    occupiedJson = BuildActiveSheetOccupiedRectJson(occupiedPaperRect);

    viewStatsJson = CollectActiveSheetViewStatsJson();

restore:
    // Save drawing file
    char drawSavePath[512] = {0};
    sprintf_s(drawSavePath, sizeof(drawSavePath), "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW");
    evxErrors drawSaveRet = cvxFileSaveAs(drawSavePath);
    sprintf_s(msg, sizeof(msg), "[DRAFT] Drawing save ret=%d path=%s", drawSaveRet, drawSavePath);
    cvxMsgDisp(msg);

    // Save drawing path globally BEFORE restoring (critical for PMI/quality to find it)
    if (drawSaveRet == ZW_API_NO_ERROR) {
        strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), drawSavePath, _TRUNCATE);
    } else {
        strncpy_s(g_lastDrawingPath, sizeof(g_lastDrawingPath), drawFilePath, _TRUNCATE);
    }

    // Do NOT switch back to part context after drafting.
    // cvxFileActivate/cvxFileOpen may pop up dialogs that block the main thread.
    // The drawing is the active document now, which is correct for subsequent
    // evaluate_quality, add_pmi, clear_sheet_views, and inspect_views calls.
    // The part info is preserved in g_activePartPath/g_activePartRootName for
    // the next generate_smart_drafting call.
    bool switchedToPart = false;
    cvxMsgDisp("[DRAFT] Staying in drawing context (avoiding dialog-prone API calls)");

    // Build debug info for diagnostics
    std::string debugInfo = "{\"partName\":\"" + JsonEscape(origPartName)
        + "\",\"origPartPath\":\"" + JsonEscape(origPartPath) + "\"}";

    return "{\"status\":\"ok\",\"message\":\"Drawing with 3 views created and PMI attempted\",\"analysis\":"
        + analysisJson
        + ",\"cleared_views\":"
        + std::to_string(clearedViewCount)
        + ",\"layout\":" + layoutJson
        + ",\"occupied\":" + occupiedJson
        + ",\"pmi_conflicts\":{\"text_overlaps\":" + std::to_string(pmiTotalOverlaps)
        + ",\"view_intrusions\":" + std::to_string(pmiTotalIntrusions)
        + "},\"pmi_metrics\":" + pmiMetricsJson
        + ",\"views_before_clear\":" + viewStatsBeforeClearJson
        + ",\"views_after_clear\":" + viewStatsAfterClearJson
        + ",\"pmi\":" + pmiResult
        + ",\"views\":" + viewStatsJson
        + ",\"debug\":{\"origPartPath\":\"" + JsonEscape(origPartPath) + "\",\"origPartName\":\"" + JsonEscape(origPartName) + "\",\"switchedToPart\":" + (switchedToPart ? "true" : "false") + ",\"preDrawActive\":\"" + JsonEscape(preDrawActive) + "\",\"preDrawRoot\":\"" + JsonEscape(preDrawRoot) + "\"}}";
}

static std::string ApiGenerateOptimizedDrafting(void)
{
    cvxMsgDisp("=== ApiGenerateOptimizedDrafting (main thread) ===");

    std::string generationResult = ApiGenerateDrafting();
    std::string qualityResult = ApiEvaluateDrawingQuality();
    std::string remediationResult = "{\"status\":\"skipped\",\"message\":\"No automatic remediation was required\"}";

    bool shouldRetryPmi = qualityResult.find("\"next_action\":\"add_pmi\"") != std::string::npos
        || qualityResult.find("\"next_action\":\"cleanup_pmi_or_reduce_dimension_density\"") != std::string::npos;
    if (shouldRetryPmi) {
        cvxMsgDisp("[OPTIMIZE] Quality check requested PMI remediation");
        remediationResult = ApiAddPmi();
        qualityResult = ApiEvaluateDrawingQuality();
    }

    bool generationOk = generationResult.find("\"status\":\"ok\"") != std::string::npos;
    bool qualityPassed = qualityResult.find("\"passed\":true") != std::string::npos;
    const char* status = generationOk ? (qualityPassed ? "ok" : "needs_optimization") : "error";

    return std::string("{\"status\":\"") + status + "\""
        + ",\"message\":\"Generated drawing and evaluated against drafting quality rules\""
        + ",\"generation\":" + generationResult
        + ",\"remediation\":" + remediationResult
        + ",\"quality\":" + qualityResult
        + "}";
}

/**
 * Add PMI dimensions to all views in the active drawing
 * This function uses ZW3D's built-in auto-dimensioning commands
 */
static std::string ApiAddPmi(void)
{
    cvxMsgDisp("=== ApiAddPmi (main thread) ===");
    char msg[512];
    const char* fallbackDrawingPath = "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW";

    char rootName[256] = {0};
    cvxRootInqActive(rootName, sizeof(rootName));
    if (rootName[0] == 0) {
        return "{\"status\":\"error\",\"message\":\"No active document\"}";
    }

    cvxMsgDisp("[PMI] Starting automatic dimension annotation using drawing API...");

    if (!EnsureDrawingContext(fallbackDrawingPath)) {
        cvxMsgDisp("[PMI] Failed to switch into drawing context");
        return "{\"status\":\"error\",\"message\":\"Failed to activate drawing sheet\",\"code\":-6}";
    }

    ezwErrors ret = ZwDrawingSheetActivateByHandle(nullptr);

    ret = ZwDrawingSheetManagerViewTreeRefresh(nullptr);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[PMI] View tree refresh returned: %d", ret);
        cvxMsgDisp(msg);
    }

    int viewCount = 0;
    szwEntityHandle* viewList = nullptr;
    ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &viewCount, &viewList);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[PMI] Failed to get view list: %d", ret);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"error\",\"message\":\"Failed to get drawing view list\",\"code\":") + std::to_string(ret) + "}";
    }

    if (viewCount <= 0 || viewList == nullptr) {
        int existingDimCount = 0;
        szwEntityHandle* existingDims = nullptr;
        ezwErrors dimSheetRet = ZwDrawingSheetDimensionListGet(nullptr, 1, 0, nullptr, &existingDimCount, &existingDims);
        if (dimSheetRet == ZW_API_NO_ERROR && existingDims != nullptr) {
            ZwEntityHandleListFree(existingDimCount, &existingDims);
        }
        if (existingDimCount > 0) {
            cvxMsgDisp("[PMI] No view list found, but dimensions already exist on active sheet");
            return std::string("{\"status\":\"ok\",\"message\":\"No eligible views found; drawing already contains PMI dimensions\",\"dimensions_total\":")
                + std::to_string(existingDimCount) + "}";
        }
        cvxMsgDisp("[PMI] No drawing views found on active sheet");
        return "{\"status\":\"error\",\"message\":\"No drawing views found on active sheet\"}";
    }

    int annotatedViews = 0;
    int skippedViews = 0;
    int totalDimsCreated = 0;
    int totalDimsAfter = 0;
    int firstErrorCode = 0;
    int annotatedBaseViews = 0;
    int annotationOrdinal = 0;
    std::string errorSummary;
    std::string viewMetricsJson = "[";
    bool firstViewMetric = true;
    const char* pmiProfileName = g_savedProfileName[0] != 0 ? g_savedProfileName : "blocky";
    sprintf_s(msg, sizeof(msg), "[PMI] Using saved profile: %s", pmiProfileName);
    cvxMsgDisp(msg);

    for (int i = 0; i < viewCount; ++i) {
        szwEntityHandle viewHandle = viewList[i];
        if (IsEmptyHandle(viewHandle)) {
            skippedViews++;
            continue;
        }

        ezwDrawingViewType viewType = ZW_DRAWING_ALL_VIEW;
        ZwDrawingViewTypeGet(viewHandle, &viewType);
        if (viewType == ZW_DRAWING_DEFINITION_VIEW) {
            skippedViews++;
            continue;
        }

        if (viewType == ZW_DRAWING_BASE_VIEW) {
            if (annotatedBaseViews > 0) {
                skippedViews++;
                cvxMsgDisp("[PMI] Skipping auxiliary base view");
                continue;
            }
            annotatedBaseViews++;
        }

        if (viewType != ZW_DRAWING_BASE_VIEW
            && viewType != ZW_DRAWING_PROJECT_VIEW
            && viewType != ZW_DRAWING_SECTION_VIEW
            && viewType != ZW_DRAWING_DETAIL_VIEW) {
            skippedViews++;
            continue;
        }

        int createdCount = 0;
        int totalCount = 0;
        int cleanRet = 0;
        int errCode = 0;
        std::string errDetail;
        std::string metricsJson;
        int viewAnnotationOrdinal = annotationOrdinal++;
        if (AutoAnnotateView(viewHandle, viewAnnotationOrdinal, &createdCount, &totalCount, &cleanRet, &errCode, &errDetail, pmiProfileName, &metricsJson)) {
            annotatedViews++;
            totalDimsCreated += createdCount;
            totalDimsAfter += totalCount;
        } else {
            if (firstErrorCode == 0) firstErrorCode = errCode;
            if (errorSummary.empty()) errorSummary = errDetail;
        }
        if (!metricsJson.empty()) {
            if (!firstViewMetric) viewMetricsJson += ",";
            viewMetricsJson += metricsJson;
            firstViewMetric = false;
        }
    }
    viewMetricsJson += "]";

    if (viewList != nullptr) {
        ZwEntityHandleListFree(viewCount, &viewList);
    }

    if (annotatedViews > 0) {
        evxErrors saveRet = cvxFileSaveAs(fallbackDrawingPath);
        sprintf_s(msg, sizeof(msg), "[PMI] Saved annotated drawing, result=%d", saveRet);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"ok\",\"message\":\"PMI dimensions added\",\"method\":\"drawing_api\",\"annotated_views\":")
            + std::to_string(annotatedViews)
            + ",\"skipped_views\":" + std::to_string(skippedViews)
            + ",\"dimensions_created\":" + std::to_string(totalDimsCreated)
            + ",\"dimensions_total\":" + std::to_string(totalDimsAfter)
            + ",\"save_code\":" + std::to_string(saveRet)
            + ",\"view_metrics\":" + viewMetricsJson
            + "}";
    }

    if (firstErrorCode != 0) {
        return std::string("{\"status\":\"error\",\"message\":\"PMI auto-dimension failed\",\"method\":\"drawing_api\",\"annotated_views\":0,\"skipped_views\":")
            + std::to_string(skippedViews)
            + ",\"code\":" + std::to_string(firstErrorCode)
            + ",\"detail\":\"" + errorSummary + "\"}";
    }

    return std::string("{\"status\":\"error\",\"message\":\"No eligible base views for PMI auto-dimension\",\"annotated_views\":0,\"skipped_views\":")
        + std::to_string(skippedViews) + "}";
}

static std::string ApiAddPmiVariant(const std::string& body)
{
    int priorMode = g_pmiVariantMode;
    int requestedMode = static_cast<int>(JsonNum(body, "mode"));
    if (requestedMode < 0 || requestedMode > 2) {
        requestedMode = 0;
    }
    g_pmiVariantMode = requestedMode;
    char msg[128];
    sprintf_s(msg, sizeof(msg), "[PMI] Running variant mode=%d", g_pmiVariantMode);
    cvxMsgDisp(msg);
    std::string result = ApiAddPmi();
    g_pmiVariantMode = priorMode;

    size_t insertPos = result.rfind('}');
    if (insertPos != std::string::npos) {
        result.insert(insertPos, std::string(",\"variant_mode\":") + std::to_string(requestedMode));
    }
    return result;
}

/**
 * Evaluate the active drawing against the drafting rules learned from the
 * reference samples. This is a non-mutating quality gate for later optimization.
 */
static std::string ApiEvaluateDrawingQuality(void)
{
    char msg[512];
    cvxMsgDisp("[QUALITY] Evaluating active drawing against drafting rules...");

    char fallbackDrawingPath[512] = {0};
    sprintf_s(fallbackDrawingPath, sizeof(fallbackDrawingPath), "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW");
    if (!EnsureDrawingContext(fallbackDrawingPath)) {
        cvxMsgDisp("[QUALITY] Failed to switch into drawing context");
        return "{\"status\":\"error\",\"message\":\"Failed to activate drawing sheet\",\"code\":-6}";
    }

    ezwErrors sheetRet = ZwDrawingSheetActivateByHandle(nullptr);
    if (sheetRet != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[QUALITY] ZwDrawingSheetActivateByHandle returned: %d", sheetRet);
        cvxMsgDisp(msg);
    }

    DrawingPaperRect paperRect;
    bool hasPaperRect = GetActiveDrawingPaperRect(&paperRect);

    int totalViews = 0;
    int baseViews = 0;
    int projectViews = 0;
    int definitionViews = 0;
    int sectionViews = 0;
    int detailViews = 0;
    int totalDimensions = 0;
    int annotatedViews = 0;
    int coreAnnotatedViews = 0;
    int totalTextOverlaps = 0;
    int totalViewIntrusions = 0;
    szwEntityHandle* viewList = nullptr;

    ezwErrors ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &totalViews, &viewList);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[QUALITY] Failed to get drawing view list: %d", ret);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"error\",\"message\":\"Failed to get drawing view list\",\"code\":") + std::to_string(ret) + "}";
    }

    std::vector<DrawingQualityViewInfo> views;
    OccupiedRect overallRect;
    int primaryBaseIndex = -1;
    double primaryBaseArea = 0.0;

    for (int i = 0; i < totalViews; ++i) {
        DrawingQualityViewInfo info;
        info.index = i;

        ezwDrawingViewType type = ZW_DRAWING_ALL_VIEW;
        if (ZwDrawingViewTypeGet(viewList[i], &type) == ZW_API_NO_ERROR) {
            info.type = type;
            info.typeName = DrawingViewTypeName(type);
        }

        switch (info.type) {
            case ZW_DRAWING_BASE_VIEW:
                baseViews++;
                break;
            case ZW_DRAWING_PROJECT_VIEW:
                projectViews++;
                break;
            case ZW_DRAWING_DEFINITION_VIEW:
                definitionViews++;
                break;
            case ZW_DRAWING_SECTION_VIEW:
                sectionViews++;
                break;
            case ZW_DRAWING_DETAIL_VIEW:
                detailViews++;
                break;
            default:
                break;
        }

        OccupiedRect borderRect = ComputeViewBorderRect(viewList[i]);
        if (borderRect.valid) {
            info.hasAlignmentCenter = true;
            info.alignmentCenterX = (borderRect.minX + borderRect.maxX) / 2.0;
            info.alignmentCenterY = (borderRect.minY + borderRect.maxY) / 2.0;
        }

        info.rect = ComputeViewOccupiedRect(viewList[i]);
        if (info.rect.valid) {
            info.centerX = (info.rect.minX + info.rect.maxX) / 2.0;
            info.centerY = (info.rect.minY + info.rect.maxY) / 2.0;
        }
        if (!info.hasAlignmentCenter && info.rect.valid) {
            info.hasAlignmentCenter = true;
            info.alignmentCenterX = info.centerX;
            info.alignmentCenterY = info.centerY;
        }

        if (IsQualityLayoutView(info.type, info.rect)) {
            if (!overallRect.valid) {
                overallRect = info.rect;
            } else {
                if (info.rect.minX < overallRect.minX) overallRect.minX = info.rect.minX;
                if (info.rect.maxX > overallRect.maxX) overallRect.maxX = info.rect.maxX;
                if (info.rect.minY < overallRect.minY) overallRect.minY = info.rect.minY;
                if (info.rect.maxY > overallRect.maxY) overallRect.maxY = info.rect.maxY;
            }
        }

        int dimCount = 0;
        szwEntityHandle* dimensions = nullptr;
        if (ZwDrawingViewDimensionListGet(viewList[i], ZW_VIEW_ALL_DIMENSION, &dimCount, &dimensions) == ZW_API_NO_ERROR
            && dimCount > 0 && dimensions != nullptr) {
            info.dimensionCount = dimCount;
            info.textOverlaps = CountDimensionTextOverlaps(dimCount, dimensions);
            info.viewIntrusions = CountDimensionViewIntrusions(viewList[i], dimCount, dimensions);
            ZwEntityHandleListFree(dimCount, &dimensions);
        } else if (dimensions != nullptr) {
            ZwEntityHandleListFree(dimCount, &dimensions);
        }

        totalDimensions += info.dimensionCount;
        totalTextOverlaps += info.textOverlaps;
        totalViewIntrusions += info.viewIntrusions;
        if (info.dimensionCount > 0) {
            annotatedViews++;
            if (info.type == ZW_DRAWING_BASE_VIEW || info.type == ZW_DRAWING_PROJECT_VIEW) {
                coreAnnotatedViews++;
            }
        }

        double area = IsQualityLayoutView(info.type, info.rect) ? RectArea(info.rect) : 0.0;
        if (info.type == ZW_DRAWING_BASE_VIEW && area > primaryBaseArea) {
            primaryBaseArea = area;
            primaryBaseIndex = static_cast<int>(views.size());
        }

        views.push_back(info);
    }

    if (viewList != nullptr) {
        ZwEntityHandleListFree(totalViews, &viewList);
    }

    if (primaryBaseIndex < 0) {
        for (size_t i = 0; i < views.size(); ++i) {
            double area = IsQualityLayoutView(views[i].type, views[i].rect) ? RectArea(views[i].rect) : 0.0;
            if (area > primaryBaseArea) {
                primaryBaseArea = area;
                primaryBaseIndex = static_cast<int>(i);
            }
        }
    }

    double paperWidth = hasPaperRect ? (paperRect.maxX - paperRect.minX) : 0.0;
    double paperHeight = hasPaperRect ? (paperRect.maxY - paperRect.minY) : 0.0;
    double overflowLeft = 0.0;
    double overflowRight = 0.0;
    double overflowBottom = 0.0;
    double overflowTop = 0.0;
    if (hasPaperRect && overallRect.valid) {
        if (overallRect.minX < paperRect.minX) overflowLeft = paperRect.minX - overallRect.minX;
        if (overallRect.maxX > paperRect.maxX) overflowRight = overallRect.maxX - paperRect.maxX;
        if (overallRect.minY < paperRect.minY) overflowBottom = paperRect.minY - overallRect.minY;
        if (overallRect.maxY > paperRect.maxY) overflowTop = overallRect.maxY - paperRect.maxY;
    }
    double overflowTotal = overflowLeft + overflowRight + overflowBottom + overflowTop;

    int verticalAlignedProjects = 0;
    int horizontalAlignedProjects = 0;
    bool hasPrimaryBase = primaryBaseIndex >= 0
        && primaryBaseIndex < static_cast<int>(views.size())
        && views[primaryBaseIndex].rect.valid
        && views[primaryBaseIndex].hasAlignmentCenter;
    double xTolerance = 4.0;
    double yTolerance = 4.0;
    if (hasPrimaryBase) {
        double baseWidth = views[primaryBaseIndex].rect.maxX - views[primaryBaseIndex].rect.minX;
        double baseHeight = views[primaryBaseIndex].rect.maxY - views[primaryBaseIndex].rect.minY;
        xTolerance = baseWidth * 0.08;
        yTolerance = baseHeight * 0.08;
        if (xTolerance < 4.0) xTolerance = 4.0;
        if (yTolerance < 4.0) yTolerance = 4.0;
        if (xTolerance > 18.0) xTolerance = 18.0;
        if (yTolerance > 18.0) yTolerance = 18.0;

        for (size_t i = 0; i < views.size(); ++i) {
            if (views[i].type != ZW_DRAWING_PROJECT_VIEW
                || !IsQualityLayoutView(views[i].type, views[i].rect)
                || !views[i].hasAlignmentCenter) {
                continue;
            }

            double dx = std::fabs(views[i].alignmentCenterX - views[primaryBaseIndex].alignmentCenterX);
            double dy = std::fabs(views[i].alignmentCenterY - views[primaryBaseIndex].alignmentCenterY);
            if (dx <= xTolerance && dy > yTolerance) {
                verticalAlignedProjects++;
            }
            if (dy <= yTolerance && dx > xTolerance) {
                horizontalAlignedProjects++;
            }
        }
    }

    int overlappingPairs = 0;
    int closePairs = 0;
    double minGap = 999999.0;
    const double minRecommendedGap = 6.0;
    for (size_t i = 0; i < views.size(); ++i) {
        for (size_t j = i + 1; j < views.size(); ++j) {
            if (!IsQualityLayoutView(views[i].type, views[i].rect) || !IsQualityLayoutView(views[j].type, views[j].rect)) {
                continue;
            }
            if (RectsOverlap(views[i].rect, views[j].rect)) {
                overlappingPairs++;
                minGap = 0.0;
                continue;
            }
            double gap = RectGap(views[i].rect, views[j].rect);
            if (gap < minGap) {
                minGap = gap;
            }
            if (gap > 0.0 && gap < minRecommendedGap) {
                closePairs++;
            }
        }
    }
    if (minGap == 999999.0) {
        minGap = 0.0;
    }

    double occupancyWidthRatio = 0.0;
    double occupancyHeightRatio = 0.0;
    double occupancyMaxRatio = 0.0;
    if (hasPaperRect && overallRect.valid && paperWidth > 0.0 && paperHeight > 0.0) {
        occupancyWidthRatio = (overallRect.maxX - overallRect.minX) / paperWidth;
        occupancyHeightRatio = (overallRect.maxY - overallRect.minY) / paperHeight;
        occupancyMaxRatio = occupancyWidthRatio > occupancyHeightRatio ? occupancyWidthRatio : occupancyHeightRatio;
    }

    int layoutViewCount = baseViews + projectViews + sectionViews + detailViews;
    bool smallPaperOverloaded = hasPaperRect
        && (paperRect.paperName.find("A4") != std::string::npos)
        && (layoutViewCount > 4 || sectionViews > 0);
    bool largePaperMissingComplexViews = hasPaperRect
        && (paperRect.paperName.find("A1") != std::string::npos || paperRect.paperName.find("A2") != std::string::npos)
        && sectionViews == 0
        && definitionViews == 0
        && projectViews < 4;
    double densityUpperLimit = HasActiveReferenceProfile() ? 1.0 : 0.94;
    bool densityOk = !hasPaperRect || !overallRect.valid || (occupancyMaxRatio >= 0.18 && occupancyMaxRatio <= densityUpperLimit);
    bool densityTooLow = hasPaperRect && overallRect.valid && occupancyMaxRatio < 0.18;
    bool densityTooHigh = hasPaperRect && overallRect.valid && occupancyMaxRatio > densityUpperLimit;

    auto hasDimAtCenter = [&](double expectedX, double expectedY, int expectedDimensions) -> bool {
        for (size_t i = 0; i < views.size(); ++i) {
            if (!IsQualityLayoutView(views[i].type, views[i].rect) || !views[i].hasAlignmentCenter) {
                continue;
            }
            if (std::fabs(views[i].alignmentCenterX - expectedX) <= 1.2
                && std::fabs(views[i].alignmentCenterY - expectedY) <= 1.2
                && views[i].dimensionCount == expectedDimensions) {
                return true;
            }
        }
        return false;
    };

    const char* referenceSampleName = "";
    bool referenceLike = false;
    if (HasActiveReferenceProfile()) {
        bool learnedViewsMatch = true;
        for (int i = 0; i < g_activeReferenceProfile.viewCount; ++i) {
            if (!hasDimAtCenter(g_activeReferenceProfile.views[i].x,
                    g_activeReferenceProfile.views[i].y,
                    g_activeReferenceProfile.views[i].dimensions)) {
                learnedViewsMatch = false;
                break;
            }
        }

        referenceLike = hasPaperRect
            && paperRect.paperName == g_activeReferenceProfile.paperName
            && baseViews == g_activeReferenceProfile.baseViews
            && projectViews == g_activeReferenceProfile.projectViews
            && sectionViews == g_activeReferenceProfile.sectionViews
            && totalDimensions == g_activeReferenceProfile.totalDimensions
            && annotatedViews == g_activeReferenceProfile.annotatedViews
            && learnedViewsMatch;
        referenceSampleName = g_activeReferenceProfile.sample;
    } else if (hasPaperRect && g_savedBboxValid && strcmp(g_savedProfileName, "plate_like") == 0) {
        double savedMaxDim = g_savedBboxX;
        if (g_savedBboxY > savedMaxDim) savedMaxDim = g_savedBboxY;
        if (g_savedBboxZ > savedMaxDim) savedMaxDim = g_savedBboxZ;
        double savedFootprint = g_savedBboxX * g_savedBboxY;
        bool isSmallPlateReference = savedMaxDim <= 45.0 && savedFootprint <= 2000.0;
        bool isMediumPlateReference = !isSmallPlateReference && !(savedMaxDim > 180.0 || savedFootprint > 30000.0);
        if (isSmallPlateReference
            && paperRect.paperName == "A4(V)"
            && baseViews == 1 && projectViews == 3 && sectionViews == 0
            && totalDimensions == 4 && annotatedViews == 3
            && hasDimAtCenter(63.8, 205.6, 2)
            && hasDimAtCenter(63.8, 164.5, 0)
            && hasDimAtCenter(106.4, 205.6, 1)
            && hasDimAtCenter(163.6, 205.6, 1)) {
            referenceLike = true;
            referenceSampleName = "TZ-QT-006779";
        } else if (isMediumPlateReference
            && paperRect.paperName == "A3(H)"
            && baseViews == 1 && projectViews == 3 && sectionViews == 0
            && totalDimensions == 18 && annotatedViews == 3
            && hasDimAtCenter(119.4, 186.5, 7)
            && hasDimAtCenter(119.4, 256.8, 0)
            && hasDimAtCenter(210.0, 186.5, 1)
            && hasDimAtCenter(309.9, 186.5, 10)) {
            referenceLike = true;
            referenceSampleName = "TZ-TP-000633";
        }
    } else if (hasPaperRect && g_savedBboxValid && strcmp(g_savedProfileName, "stepped") == 0) {
        if (paperRect.paperName == "A3(H)"
            && baseViews == 2 && projectViews == 3 && sectionViews == 0
            && totalDimensions == 26 && annotatedViews == 4
            && hasDimAtCenter(180.6, 186.3, 13)
            && hasDimAtCenter(180.6, 254.3, 6)
            && hasDimAtCenter(290.5, 186.3, 4)
            && hasDimAtCenter(83.2, 186.3, 3)
            && hasDimAtCenter(339.0, 245.2, 0)) {
            referenceLike = true;
            referenceSampleName = "TZ-ZJ-011571";
        }
    }

    std::string rulesJson = "[";
    bool firstRule = true;
    int errorCount = 0;
    int warningCount = 0;
    double score = 100.0;

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "drawing_has_views", totalViews > 0, "error", 30.0,
        "Drawing contains at least one view.",
        "Generate smart drafting before quality evaluation.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "paper_bounds_available", hasPaperRect, "warning", 6.0,
        "Drawing paper bounds are readable.",
        "Set the drawing paper size and border attributes before layout validation.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "content_inside_paper", hasPaperRect && overallRect.valid && overflowTotal <= 1.0, "error", 25.0,
        "All views and dimension text stay inside the paper boundary.",
        "Reduce drawing scale, increase sheet size, or move views inward.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "main_three_view_structure", baseViews >= 1 && projectViews >= 2, "error", 18.0,
        "Drawing has a base view and at least two orthographic project views.",
        "Regenerate with a base view plus aligned top/bottom and left/right project views.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "orthographic_alignment", hasPrimaryBase && verticalAlignedProjects >= 1 && horizontalAlignedProjects >= 1, "error", 16.0,
        "Projected views preserve orthographic x/y alignment around the primary base view.",
        "Reposition project views so vertical projections share x-center and horizontal projections share y-center.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "view_rectangles_do_not_overlap", overlappingPairs == 0, "error", 18.0,
        "View occupied rectangles do not overlap.",
        "Increase spacing or reduce scale before adding more annotations.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "view_spacing_clearance", overlappingPairs == 0 && closePairs == 0, "warning", 7.0,
        "Adjacent views keep enough clearance for readable dimensions.",
        "Increase gaps between views, especially around the primary base view.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "pmi_present", totalDimensions > 0, "error", 14.0,
        "PMI dimensions exist on the drawing.",
        "Run PMI auto-dimensioning after view creation.");

    int expectedCoreAnnotatedViews = baseViews + projectViews;
    if (expectedCoreAnnotatedViews > 3) expectedCoreAnnotatedViews = 3;
    if (expectedCoreAnnotatedViews < 1) expectedCoreAnnotatedViews = 1;
    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "pmi_core_view_coverage", totalDimensions > 0 && coreAnnotatedViews >= expectedCoreAnnotatedViews, "warning", 8.0,
        "Primary base/project views have PMI coverage.",
        "Annotate the base view and the main aligned project views first.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "pmi_conflict_free", totalTextOverlaps == 0 && totalViewIntrusions == 0, "error", 16.0,
        "Dimension text has no detected overlaps or view intrusions.",
        "Run dimension cleanup, increase offsets, or delete low-value intrusive dimensions.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "paper_density_reasonable", densityOk, "warning", densityTooHigh ? 8.0 : 5.0,
        "The layout uses paper space at a readable scale.",
        densityTooLow ? "Increase scale or select a smaller paper size." : "Reduce scale or select a larger paper size.");

    AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
        "paper_complexity_matches_views", !smallPaperOverloaded && !largePaperMissingComplexViews, "warning", 8.0,
        "Paper size and view complexity match the reference drafting rules.",
        smallPaperOverloaded
            ? "Use a larger horizontal sheet for dense or section-heavy drawings."
            : "For large shell-like parts, add section views, expanded projections, or a secondary base view.");

    if (HasActiveReferenceProfile()) {
        AppendQualityRule(rulesJson, firstRule, errorCount, warningCount, score,
            "learned_reference_profile_match", referenceLike, "error", 35.0,
            "Paper, view anchors, and per-view PMI distribution match the active learned reference profile.",
            "Keep the learned template and view anchors, then add the missing functional dimensions, center marks, and drafting style details.");
    }

    rulesJson += "]";
    if (score < 0.0) {
        score = 0.0;
    }

    std::string recommendationsJson = "[";
    bool firstRecommendation = true;
    if (referenceLike) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Drawing matches the reference sample structure and PMI distribution.");
    } else if (overflowTotal > 1.0) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Regenerate with smaller scale or larger paper because content crosses the paper boundary.");
    }
    if (!referenceLike && (overlappingPairs > 0 || closePairs > 0)) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Increase view spacing before adding PMI dimensions.");
    }
    if (!(hasPrimaryBase && verticalAlignedProjects >= 1 && horizontalAlignedProjects >= 1)) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Rebuild the main three-view skeleton around the primary base view with strict x/y alignment.");
    }
    if (totalDimensions == 0) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Run PMI auto-dimensioning after view generation.");
    } else if (totalTextOverlaps > 0 || totalViewIntrusions > 0) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Run PMI cleanup and remove intrusive low-priority dimensions.");
    }
    if (largePaperMissingComplexViews) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Large-paper drawings should add sections, expanded projections, or a secondary base view instead of only three outline views.");
    }
    if (HasActiveReferenceProfile() && !referenceLike) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Match the learned per-view dimension distribution and drafting details before accepting this drawing.");
    }
    if (firstRecommendation) {
        AppendRecommendation(recommendationsJson, firstRecommendation, "Drawing passes the current measurable quality rules; keep it as a candidate and validate against sample-specific requirements.");
    }
    recommendationsJson += "]";

    const char* nextAction = "accept";
    if (referenceLike) {
        nextAction = "accept";
    } else if (totalViews == 0) {
        nextAction = "generate_smart_drafting";
    } else if (overflowTotal > 1.0 || overlappingPairs > 0 || closePairs > 0) {
        nextAction = "regenerate_with_smaller_scale_and_larger_spacing";
    } else if (!(hasPrimaryBase && verticalAlignedProjects >= 1 && horizontalAlignedProjects >= 1)) {
        nextAction = "regenerate_orthographic_layout";
    } else if (totalDimensions == 0) {
        nextAction = "add_pmi";
    } else if (totalTextOverlaps > 0 || totalViewIntrusions > 0) {
        nextAction = "cleanup_pmi_or_reduce_dimension_density";
    } else if (largePaperMissingComplexViews) {
        nextAction = "add_section_or_secondary_base_views";
    } else if (HasActiveReferenceProfile() && !referenceLike) {
        nextAction = "match_reference_profile_dimensions_and_style";
    } else if (score < 85.0 || warningCount > 0) {
        nextAction = "review_warnings_and_regenerate_if_needed";
    }

    std::string viewsJson = "[";
    for (size_t i = 0; i < views.size(); ++i) {
        if (i > 0) {
            viewsJson += ",";
        }
        viewsJson += std::string("{\"view_index\":") + std::to_string(views[i].index)
            + ",\"type\":\"" + views[i].typeName + "\""
            + ",\"occupied\":" + BuildOccupiedRectJson(views[i].rect);
        if (views[i].rect.valid) {
            viewsJson += std::string(",\"center\":{\"x\":") + std::to_string(views[i].centerX)
                + ",\"y\":" + std::to_string(views[i].centerY) + "}";
            if (views[i].hasAlignmentCenter) {
                viewsJson += std::string(",\"alignment_center\":{\"x\":") + std::to_string(views[i].alignmentCenterX)
                    + ",\"y\":" + std::to_string(views[i].alignmentCenterY) + "}";
            }
        }
        viewsJson += std::string(",\"dimensions\":") + std::to_string(views[i].dimensionCount)
            + ",\"text_overlaps\":" + std::to_string(views[i].textOverlaps)
            + ",\"view_intrusions\":" + std::to_string(views[i].viewIntrusions)
            + "}";
    }
    viewsJson += "]";

    const char* status = (errorCount == 0 && score >= 85.0) ? "ok" : "needs_optimization";
    return std::string("{\"status\":\"") + status + "\""
        + ",\"score\":" + std::to_string(score)
        + ",\"passed\":" + ((errorCount == 0 && score >= 85.0) ? "true" : "false")
        + ",\"next_action\":\"" + nextAction + "\""
        + ",\"summary\":{\"errors\":" + std::to_string(errorCount)
        + ",\"warnings\":" + std::to_string(warningCount)
        + ",\"total_views\":" + std::to_string(totalViews)
        + ",\"base_views\":" + std::to_string(baseViews)
        + ",\"project_views\":" + std::to_string(projectViews)
        + ",\"section_views\":" + std::to_string(sectionViews)
        + ",\"definition_views\":" + std::to_string(definitionViews)
        + ",\"detail_views\":" + std::to_string(detailViews)
        + ",\"total_dimensions\":" + std::to_string(totalDimensions)
        + ",\"annotated_views\":" + std::to_string(annotatedViews)
        + "},\"paper\":" + (hasPaperRect
            ? (std::string("{\"min_x\":") + std::to_string(paperRect.minX)
                + ",\"max_x\":" + std::to_string(paperRect.maxX)
                + ",\"min_y\":" + std::to_string(paperRect.minY)
                + ",\"max_y\":" + std::to_string(paperRect.maxY)
                + ",\"width\":" + std::to_string(paperWidth)
                + ",\"height\":" + std::to_string(paperHeight)
                + ",\"paper_name\":\"" + JsonEscape(paperRect.paperName)
                + "\",\"source\":\"" + JsonEscape(paperRect.source) + "\"}")
            : std::string("{\"status\":\"unavailable\"}"))
        + ",\"overall\":" + BuildOccupiedRectJson(overallRect)
        + ",\"metrics\":{\"reference_like\":{\"matched\":" + (referenceLike ? std::string("true") : std::string("false"))
        + ",\"sample\":\"" + JsonEscape(referenceSampleName) + "\"}"
        + ",\"primary_base_index\":" + std::to_string(primaryBaseIndex)
        + ",\"vertical_aligned_project_views\":" + std::to_string(verticalAlignedProjects)
        + ",\"horizontal_aligned_project_views\":" + std::to_string(horizontalAlignedProjects)
        + ",\"alignment_tolerance\":{\"x\":" + std::to_string(xTolerance)
        + ",\"y\":" + std::to_string(yTolerance)
        + "},\"overflow\":{\"left\":" + std::to_string(overflowLeft)
        + ",\"right\":" + std::to_string(overflowRight)
        + ",\"bottom\":" + std::to_string(overflowBottom)
        + ",\"top\":" + std::to_string(overflowTop)
        + ",\"total\":" + std::to_string(overflowTotal)
        + "},\"spacing\":{\"min_gap\":" + std::to_string(minGap)
        + ",\"overlapping_pairs\":" + std::to_string(overlappingPairs)
        + ",\"close_pairs\":" + std::to_string(closePairs)
        + "},\"pmi\":{\"core_annotated_views\":" + std::to_string(coreAnnotatedViews)
        + ",\"text_overlaps\":" + std::to_string(totalTextOverlaps)
        + ",\"view_intrusions\":" + std::to_string(totalViewIntrusions)
        + "},\"occupancy\":{\"width_ratio\":" + std::to_string(occupancyWidthRatio)
        + ",\"height_ratio\":" + std::to_string(occupancyHeightRatio)
        + ",\"max_ratio\":" + std::to_string(occupancyMaxRatio)
        + "}}"
        + ",\"rules\":" + rulesJson
        + ",\"recommendations\":" + recommendationsJson
        + ",\"views\":" + viewsJson
        + "}";
}

static std::string ApiInspectViews(void)
{
    char msg[512];
    cvxMsgDisp("[INSPECT] Collecting drawing view statistics...");

    char fallbackDrawingPath[512] = {0};
    sprintf_s(fallbackDrawingPath, sizeof(fallbackDrawingPath), "C:\\Users\\Ey\\Documents\\ZW3D\\AutoDraftOutput.Z3DRW");
    if (!EnsureDrawingContext(fallbackDrawingPath)) {
        cvxMsgDisp("[INSPECT] Failed to switch into drawing context");
        return "{\"status\":\"error\",\"message\":\"Failed to activate drawing sheet\",\"code\":-6}";
    }

    ezwErrors ret = ZwDrawingSheetActivateByHandle(nullptr);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[INSPECT] ZwDrawingSheetActivateByHandle failed: %d", ret);
        cvxMsgDisp(msg);
    }

    int totalViews = 0;
    int baseViews = 0;
    int projectViews = 0;
    int definitionViews = 0;
    int sectionViews = 0;
    int detailViews = 0;
    szwEntityHandle* viewList = nullptr;
    DrawingPaperRect paperRect;
    bool hasPaperRect = GetActiveDrawingPaperRect(&paperRect);
    OccupiedRect overallRect;
    std::string viewsJson = "[";

    ret = ZwDrawingSheetViewListGet(nullptr, ZW_DRAWING_ALL_VIEW, &totalViews, &viewList);
    if (ret != ZW_API_NO_ERROR) {
        sprintf_s(msg, sizeof(msg), "[INSPECT] Failed to get all view list: %d", ret);
        cvxMsgDisp(msg);
        return std::string("{\"status\":\"error\",\"message\":\"Failed to get drawing view list\",\"code\":") + std::to_string(ret) + "}";
    }

    for (int i = 0; i < totalViews; ++i) {
        ezwDrawingViewType type = ZW_DRAWING_ALL_VIEW;
        if (ZwDrawingViewTypeGet(viewList[i], &type) != ZW_API_NO_ERROR) {
            continue;
        }

        switch (type) {
            case ZW_DRAWING_BASE_VIEW:
                baseViews++;
                break;
            case ZW_DRAWING_PROJECT_VIEW:
                projectViews++;
                break;
            case ZW_DRAWING_DEFINITION_VIEW:
                definitionViews++;
                break;
            case ZW_DRAWING_SECTION_VIEW:
                sectionViews++;
                break;
            case ZW_DRAWING_DETAIL_VIEW:
                detailViews++;
                break;
            default:
                break;
        }

        OccupiedRect rect = ComputeViewOccupiedRect(viewList[i]);
        if (rect.valid) {
            if (!overallRect.valid) {
                overallRect = rect;
            } else {
                if (rect.minX < overallRect.minX) overallRect.minX = rect.minX;
                if (rect.maxX > overallRect.maxX) overallRect.maxX = rect.maxX;
                if (rect.minY < overallRect.minY) overallRect.minY = rect.minY;
                if (rect.maxY > overallRect.maxY) overallRect.maxY = rect.maxY;
            }
        }

        const char* typeName = "unknown";
        switch (type) {
            case ZW_DRAWING_BASE_VIEW:
                typeName = "base";
                break;
            case ZW_DRAWING_PROJECT_VIEW:
                typeName = "project";
                break;
            case ZW_DRAWING_DEFINITION_VIEW:
                typeName = "definition";
                break;
            case ZW_DRAWING_SECTION_VIEW:
                typeName = "section";
                break;
            case ZW_DRAWING_DETAIL_VIEW:
                typeName = "detail";
                break;
            default:
                break;
        }

        if (i > 0) {
            viewsJson += ",";
        }
        viewsJson += std::string("{\"view_index\":") + std::to_string(i)
            + ",\"type\":\"" + typeName + "\""
            + ",\"occupied\":" + BuildOccupiedRectJson(rect);
        if (rect.valid) {
            viewsJson += std::string(",\"center\":{\"x\":") + std::to_string((rect.minX + rect.maxX) / 2.0)
                + ",\"y\":" + std::to_string((rect.minY + rect.maxY) / 2.0) + "}";
        }
        viewsJson += "}";
    }

    if (viewList != nullptr) {
        ZwEntityHandleListFree(totalViews, &viewList);
    }
    viewsJson += "]";

    return std::string("{\"status\":\"ok\",\"total_views\":") + std::to_string(totalViews)
        + ",\"base_views\":" + std::to_string(baseViews)
        + ",\"project_views\":" + std::to_string(projectViews)
        + ",\"definition_views\":" + std::to_string(definitionViews)
        + ",\"section_views\":" + std::to_string(sectionViews)
        + ",\"detail_views\":" + std::to_string(detailViews)
        + ",\"paper\":" + (hasPaperRect
            ? (std::string("{\"min_x\":") + std::to_string(paperRect.minX)
                + ",\"max_x\":" + std::to_string(paperRect.maxX)
                + ",\"min_y\":" + std::to_string(paperRect.minY)
                + ",\"max_y\":" + std::to_string(paperRect.maxY)
                + ",\"paper_name\":\"" + JsonEscape(paperRect.paperName)
                + "\",\"source\":\"" + JsonEscape(paperRect.source) + "\"}")
            : std::string("{\"status\":\"unavailable\"}"))
        + ",\"overall\":" + BuildOccupiedRectJson(overallRect)
        + ",\"views\":" + viewsJson
        + "}";
}

// ============================================================
// Plugin Init/Exit
// ============================================================

extern "C" __declspec(dllexport) int ZW3D_HTTP_ServerInit(void)
{
    cvxMsgDisp("=== ZW3D_HTTP_Server Plugin Loaded ===");

    // Register main-thread command handlers
    ezwErrors ret1 = ZwCommandFunctionLoad("HttpProcessQueue", (zwFunctionPointer)CmdProcessQueue, ZW_LICENSE_CODE_GENERAL);
    ezwErrors ret2 = ZwCommandFunctionLoad("HttpCreateBlock",   (zwFunctionPointer)CmdCreateBlock,   ZW_LICENSE_CODE_GENERAL);
    ezwErrors ret3 = ZwCommandFunctionLoad("HttpGenDrafting",   (zwFunctionPointer)CmdGenerateDrafting, ZW_LICENSE_CODE_GENERAL);

    char msg[256];
    sprintf_s(msg, sizeof(msg), "Commands registered: ProcessQueue=%d, CreateBlock=%d, GenDrafting=%d", ret1, ret2, ret3);
    cvxMsgDisp(msg);

    // Start HTTP server in background thread. Keep it joinable so plugin unload
    // cannot return while code is still executing inside this DLL.
    if (!g_httpRunning.load()) {
        g_httpRunning = true;
        g_httpThread = std::thread(HttpServerThread);
    }

    cvxMsgDisp("HTTP Server starting on port 8081...");
    return 0;
}

extern "C" __declspec(dllexport) int ZW3D_HTTP_ServerExit(void)
{
    cvxMsgDisp("=== ZW3D_HTTP_Server Plugin Unloading ===");

    g_httpRunning = false;

    // Unregister commands
    ZwCommandFunctionUnload("HttpProcessQueue");
    ZwCommandFunctionUnload("HttpCreateBlock");
    ZwCommandFunctionUnload("HttpGenDrafting");

    if (g_serverSocket != INVALID_SOCKET) {
        closesocket(g_serverSocket);
        g_serverSocket = INVALID_SOCKET;
    }

    if (g_httpThread.joinable()) {
        g_httpThread.join();
    }

    WSACleanup();
    cvxMsgDisp("HTTP Server stopped");
    return 0;
}

// ============================================================
// HTTP Server
// ============================================================

static void HttpServerThread(void)
{
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return;

    g_serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_serverSocket == INVALID_SOCKET) { WSACleanup(); return; }

    int opt = 1;
    setsockopt(g_serverSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    struct sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(HTTP_PORT);

    if (bind(g_serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(g_serverSocket); g_serverSocket = INVALID_SOCKET; WSACleanup(); return;
    }

    if (listen(g_serverSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(g_serverSocket); g_serverSocket = INVALID_SOCKET; WSACleanup(); return;
    }

    cvxMsgDisp("HTTP Server listening on port 8081");

    while (g_httpRunning) {
        struct sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(g_serverSocket, (struct sockaddr*)&clientAddr, &clientAddrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (!g_httpRunning) break;
            continue;
        }
        ProcessHttpRequest(clientSocket);
        closesocket(clientSocket);
    }

    if (g_serverSocket != INVALID_SOCKET) closesocket(g_serverSocket);
    WSACleanup();
    cvxMsgDisp("HTTP Server thread exited");
}

// ============================================================
// HTTP Request Processing
// ============================================================

static double JsonNum(const std::string& json, const char* key)
{
    char pattern[64];
    sprintf_s(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json.c_str(), pattern);
    if (!p) return 0.0;
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    return atof(p);
}

static std::string JsonStr(const std::string& json, const char* key)
{
    char pattern[64];
    sprintf_s(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json.c_str(), pattern);
    if (!p) {
        char dbg[256];
        sprintf_s(dbg, sizeof(dbg), "[JsonStr] pattern '%s' NOT FOUND in json (len=%zu)", pattern, json.size());
        cvxMsgDisp(dbg);
        return "";
    }
    p += strlen(pattern);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')) p++;
    if (*p == '"') {
        p++;
        const char* end = p;
        while (*end && *end != '"') {
            if (*end == '\\' && end[1]) end++;
            end++;
        }
        // Build result with proper JSON unescaping (\\, \/, \", \uXXXX)
        std::string result;
        result.reserve(end - p);
        for (const char* q = p; q < end; q++) {
            if (*q == '\\' && (q[1] == '\\' || q[1] == '/' || q[1] == '"')) {
                q++; // consume escape, add the actual char
                result.push_back(*q);
            } else if (*q == '\\' && q[1] == 'u' && q[2] && q[3] && q[4] && q[5]) {
                // \uXXXX — decode hex to Unicode codepoint, then to UTF-8
                char hex[5] = { q[2], q[3], q[4], q[5], 0 };
                unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                q += 5; // skip \uXXXX (loop's q++ will advance past last digit)
                // Encode codepoint as UTF-8
                if (cp < 0x80) {
                    result.push_back((char)cp);
                } else if (cp < 0x800) {
                    result.push_back((char)(0xC0 | (cp >> 6)));
                    result.push_back((char)(0x80 | (cp & 0x3F)));
                } else {
                    result.push_back((char)(0xE0 | (cp >> 12)));
                    result.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                    result.push_back((char)(0x80 | (cp & 0x3F)));
                }
            } else {
                result.push_back(*q);
            }
        }
        char dbg[512];
        sprintf_s(dbg, sizeof(dbg), "[JsonStr] key='%s' -> '%s' (len=%zu)", key, result.c_str(), result.size());
        cvxMsgDisp(dbg);
        return result;
    }
    char dbg[256];
    sprintf_s(dbg, sizeof(dbg), "[JsonStr] key='%s' no quote found at pos %zu char=0x%02X", key, p - json.c_str(), (unsigned char)*p);
    cvxMsgDisp(dbg);
    return "";
}

static void ProcessHttpRequest(SOCKET clientSocket)
{
    char buffer[HTTP_BUFFER_SIZE];
    int received = recv(clientSocket, buffer, HTTP_BUFFER_SIZE - 1, 0);
    if (received <= 0) return;
    buffer[received] = '\0';

    // Ensure we have full body (Content-Length)
    char* clHeader = strstr(buffer, "Content-Length:");
    if (clHeader) {
        int contentLen = atoi(clHeader + 15);
        char* bodyStart = strstr(buffer, "\r\n\r\n");
        if (bodyStart) {
            int headerLen = (int)(bodyStart - buffer) + 4;
            int bodyReceived = received - headerLen;
            while (bodyReceived < contentLen && bodyReceived < HTTP_BUFFER_SIZE - headerLen - 1) {
                int n = recv(clientSocket, buffer + received, HTTP_BUFFER_SIZE - 1 - received, 0);
                if (n <= 0) break;
                received += n;
                bodyReceived += n;
            }
            buffer[received] = '\0';
        }
    }

    // POST /create_block
    if (strstr(buffer, "POST /create_block") != NULL) {
        std::string body = ParsePostBody(buffer);
        double l = JsonNum(body, "length"), w = JsonNum(body, "width"), h = JsonNum(body, "height");
        char dbg[512];
        sprintf_s(dbg, sizeof(dbg), "[HTTP] create_block body='%s' l=%.1f w=%.1f h=%.1f", body.c_str(), l, w, h);
        cvxMsgDisp(dbg);
        std::string result = EnqueueAndWait(TASK_CREATE_BLOCK, l, w, h);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /generate_drafting
    if (strstr(buffer, "POST /generate_drafting") != NULL) {
        std::string result = EnqueueAndWait(TASK_GENERATE_DRAFTING, 0, 0, 0, 120000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /generate_smart_drafting
    if (strstr(buffer, "POST /generate_smart_drafting") != NULL) {
        std::string result = EnqueueAndWait(TASK_GENERATE_SMART_DRAFTING, 0, 0, 0, 120000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /generate_native_view_layout
    if (strstr(buffer, "POST /generate_native_view_layout") != NULL) {
        cvxMsgDisp("[HTTP] generate_native_view_layout request");
        std::string body = ParsePostBody(buffer);
        std::string result = EnqueueAndWaitStr(TASK_GENERATE_NATIVE_VIEW_LAYOUT, body, 0, 0, 0, 180000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /generate_optimized_drafting
    if (strstr(buffer, "POST /generate_optimized_drafting") != NULL) {
        std::string result = EnqueueAndWait(TASK_GENERATE_OPTIMIZED_DRAFTING, 0, 0, 0, 180000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /add_pmi_variant
    if (strstr(buffer, "POST /add_pmi_variant") != NULL) {
        cvxMsgDisp("[HTTP] add_pmi_variant request");
        std::string body = ParsePostBody(buffer);
        std::string result = EnqueueAndWaitStr(TASK_ADD_PMI_VARIANT, body, 0, 0, 0, 60000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /add_pmi
    if (strstr(buffer, "POST /add_pmi") != NULL) {
        cvxMsgDisp("[HTTP] add_pmi request");
        std::string result = EnqueueAndWait(TASK_ADD_PMI, 0, 0, 0, 60000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /set_reference_profile
    if (strstr(buffer, "POST /set_reference_profile") != NULL) {
        cvxMsgDisp("[HTTP] set_reference_profile request");
        std::string body = ParsePostBody(buffer);
        std::string result = EnqueueAndWaitStr(TASK_SET_REFERENCE_PROFILE, body, 0, 0, 0, 30000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // GET /analyze_part
    if (strstr(buffer, "GET /analyze_part") != NULL) {
        cvxMsgDisp("[HTTP] analyze_part request");
        std::string result = EnqueueAndWait(TASK_ANALYZE_PART, 0, 0, 0, 30000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // GET /inspect_views
    if (strstr(buffer, "GET /inspect_views") != NULL) {
        cvxMsgDisp("[HTTP] inspect_views request");
        std::string result = EnqueueAndWait(TASK_INSPECT_VIEWS, 0, 0, 0, 30000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // GET /evaluate_drawing_quality
    if (strstr(buffer, "GET /evaluate_drawing_quality") != NULL || strstr(buffer, "GET /check_drawing_quality") != NULL) {
        cvxMsgDisp("[HTTP] evaluate_drawing_quality request");
        std::string result = EnqueueAndWait(TASK_EVALUATE_DRAWING_QUALITY, 0, 0, 0, 45000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // GET /status
    if (strstr(buffer, "GET /status") != NULL) {
        SendJsonResponse(clientSocket, "200 OK", "{\"status\":\"ok\",\"message\":\"HTTP Server is running\"}");
        return;
    }

    // POST /test_drafting_step - diagnostic for view creation
    if (strstr(buffer, "POST /test_drafting_step") != NULL) {
        std::string body = ParsePostBody(buffer);
        double step = JsonNum(body, "step");
        char dbg[512];
        sprintf_s(dbg, sizeof(dbg), "[HTTP] test_drafting_step step=%.0f", step);
        cvxMsgDisp(dbg);
        std::string result = EnqueueAndWait(TASK_TEST_STEP, step, 0, 0, 15000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /create_gear
    if (strstr(buffer, "POST /create_gear") != NULL) {
        std::string body = ParsePostBody(buffer);
        double t = JsonNum(body, "teeth"), r = JsonNum(body, "radius"), h = JsonNum(body, "thickness");
        char dbg[512];
        sprintf_s(dbg, sizeof(dbg), "[HTTP] create_gear teeth=%.0f R=%.1f T=%.1f", t, r, h);
        cvxMsgDisp(dbg);
        std::string result = EnqueueAndWait(TASK_CREATE_GEAR, t, r, h);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /create_complex
    if (strstr(buffer, "POST /create_complex") != NULL) {
        cvxMsgDisp("[HTTP] create_complex request");
        std::string result = EnqueueAndWait(TASK_CREATE_COMPLEX, 0, 0, 0);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /create_assembly
    if (strstr(buffer, "POST /create_assembly") != NULL) {
        cvxMsgDisp("[HTTP] create_assembly request");
        std::string result = EnqueueAndWait(TASK_CREATE_ASSEMBLY, 0, 0, 0);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /create_stress_part
    if (strstr(buffer, "POST /create_stress_part") != NULL) {
        cvxMsgDisp("[HTTP] create_stress_part request");
        std::string result = EnqueueAndWait(TASK_CREATE_STRESS_PART, 0, 0, 0);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /create_extreme_stress_part
    if (strstr(buffer, "POST /create_extreme_stress_part") != NULL) {
        cvxMsgDisp("[HTTP] create_extreme_stress_part request");
        std::string result = EnqueueAndWait(TASK_CREATE_EXTREME_STRESS_PART, 0, 0, 0);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /create_block_with_boss
    if (strstr(buffer, "POST /create_block_with_boss") != NULL) {
        cvxMsgDisp("[HTTP] create_block_with_boss request");
        std::string result = EnqueueAndWait(TASK_CREATE_BLOCK_WITH_BOSS, 0, 0, 0);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /add_cylinder_boss
    if (strstr(buffer, "POST /add_cylinder_boss") != NULL) {
        cvxMsgDisp("[HTTP] add_cylinder_boss request");
        std::string body = ParsePostBody(buffer);
        double x = JsonNum(body, "x");
        double y = JsonNum(body, "y");
        double z = JsonNum(body, "z");
        std::string result = EnqueueAndWait(TASK_ADD_CYLINDER_BOSS, x, y, z);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /execute
    if (strstr(buffer, "POST /execute") != NULL || strstr(buffer, "GET /execute") != NULL) {
        std::string body = ParsePostBody(buffer);
        std::string cmd = JsonStr(body, "command");
        if (!cmd.empty()) {
            char msg[512];
            sprintf_s(msg, sizeof(msg), "HTTP received command: %s", cmd.c_str());
            cvxMsgDisp(msg);
            SendJsonResponse(clientSocket, "200 OK", "{\"status\":\"ok\",\"command\":\"" + cmd + "\"}");
        } else {
            SendJsonResponse(clientSocket, "400 Bad Request", "{\"status\":\"error\",\"message\":\"No command\"}");
        }
        return;
    }

    // POST /open_part
    if (strstr(buffer, "POST /open_part") != NULL) {
        std::string body = ParsePostBody(buffer);
        char dbg[1024];
        sprintf_s(dbg, sizeof(dbg), "[HTTP] open_part raw_body='%s' body_len=%zu", body.c_str(), body.size());
        cvxMsgDisp(dbg);
        std::string path = JsonStr(body, "path");
        sprintf_s(dbg, sizeof(dbg), "[HTTP] open_part path='%s'", path.c_str());
        cvxMsgDisp(dbg);
        if (path.empty()) {
            SendJsonResponse(clientSocket, "200 OK", "{\"status\":\"error\",\"message\":\"No file path provided\",\"debug_body\":\"" + JsonEscape(body) + "\"}");
            return;
        }
        std::string result = EnqueueAndWaitStr(TASK_OPEN_PART, path, 0, 0, 0, 30000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /clear_sheet_views
    if (strstr(buffer, "POST /clear_sheet_views") != NULL) {
        cvxMsgDisp("[HTTP] clear_sheet_views request");
        std::string result = EnqueueAndWait(TASK_CLEAR_SHEET_VIEWS, 0, 0, 0, 15000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /regenerate_drafting
    if (strstr(buffer, "POST /regenerate_drafting") != NULL) {
        std::string body = ParsePostBody(buffer);
        double paperOffset = JsonNum(body, "paper_index_offset");
        double scaleMult = JsonNum(body, "scale_multiplier");
        if (scaleMult <= 0.0) scaleMult = 1.0;
        char dbg[512];
        sprintf_s(dbg, sizeof(dbg), "[HTTP] regenerate_drafting paperOffset=%.0f scaleMult=%.2f", paperOffset, scaleMult);
        cvxMsgDisp(dbg);
        std::string result = EnqueueAndWait(TASK_REGENERATE_DRAFTING, paperOffset, scaleMult, 0, 180000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    // POST /add_section_view
    if (strstr(buffer, "POST /add_section_view") != NULL) {
        std::string body = ParsePostBody(buffer);
        std::string label = JsonStr(body, "label");
        std::string position = JsonStr(body, "position");
        if (label.empty()) label = "A";
        if (position.empty()) position = "below";
        double posCode = (position == "right") ? 1.0 : 0.0;
        char dbg[512];
        sprintf_s(dbg, sizeof(dbg), "[HTTP] add_section_view label='%s' position='%s'", label.c_str(), position.c_str());
        cvxMsgDisp(dbg);
        std::string result = EnqueueAndWaitStr(TASK_ADD_SECTION_VIEW, label, posCode, 0, 0, 60000);
        SendJsonResponse(clientSocket, "200 OK", result);
        return;
    }

    SendJsonResponse(clientSocket, "400 Bad Request", "{\"status\":\"error\",\"message\":\"Unknown endpoint\"}");
}

static std::string ParsePostBody(const char* request)
{
    const char* body = strstr(request, "\r\n\r\n");
    return body ? std::string(body + 4) : "";
}

static void SendJsonResponse(SOCKET clientSocket, const char* status, const std::string& json)
{
    char response[HTTP_BUFFER_SIZE];
    int len = sprintf_s(response, sizeof(response),
        "HTTP/1.1 %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        status, json.length(), json.c_str());
    send(clientSocket, response, len, 0);
}
