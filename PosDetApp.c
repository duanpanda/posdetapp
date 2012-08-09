/*=============================================================================
  FILE: PosDetApp.c
  ============================================================================*/

/*-----------------------------------------------------------------------------
  Includes and Variable Definitions
  ----------------------------------------------------------------------------*/
#include "AEEModGen.h"          // Module interface definitions.
#include "AEEAppGen.h"          // Applet interface definitions.
#include "AEEShell.h"           // Shell interface definitions.
#include "AEEPosDet.h"
#include "AEEStdLib.h"
#include "AEEFile.h"

#include "CPosDetApp.h"
#include "RyanUtils.h"
#include "PosDetApp_res.h"

typedef enum {
    SINGLE_REQUEST,
    MULTIPLE_REQUESTS
} RequestType;

typedef struct _CSettings
{
    AEEGPSOpt    optim;
    AEEGPSQos    qos;
    AEEGPSServer server;
    RequestType  reqType;
} CSettings;

typedef enum {
    TRACK_LOCAL,      /* Uses AEEGPS_TRACK_LOCAL */
    TRACK_NETWORK,    /* Uses AEEGPS_TRACK_NETOWORK */
    TRACK_AUTO        /* Attempts using AEEGPS_TRACK_LOCAL if it fails uses
                         AEEGPS_TRACK_NETWORK */
} TrackType;

typedef struct {
    int nErr;         /* SUCCESS or AEEGPS_ERR_* */
    uint32 dwFixNum;  /* fix number in this tracking session. */
    double lat;       /* latitude on WGS-84 Geoid */
    double lon;       /* longitude on WGS-84 Geoid */
    short  height;    /* Height from WGS-84 Geoid */
    AEEGPSServer server;
    AEEGPSQos qos;
    AEEGPSOpt optim;
} PositionData;

typedef struct _TrackState{
    boolean bInNotification; /* When the state machine is notifying the
                                client */
    boolean bSetForCancellation; /* Track is meant to be canceled. do so when
                                    it is safe. */
    boolean bInProgress;        /* when tracking is in progress. */
    /**************************/

    /* For TRACK_AUTO */
    boolean bModeAuto;
    boolean bModeLocal;

    /* Private members to work with IPosDet */
    AEECallback cbIntervalTimer;
    AEECallback cbInfo;
    AEEGPSInfo theInfo;
    /**************************/

    /* Clients response members. */
    AEECallback *pcbResp;
    PositionData *pResp;
    /**************************/

    /* Client passed members. */
    int nPendingFixes;
    int nTrackInterval;
    IPosDet *pPos;
    IShell *pShell;
    /**************************/
} TrackState;

typedef struct _PosDetApp {
    AEEApplet applet;     // First element of this structure must be AEEApplet.
    AEEDeviceInfo deviceInfo;   // Copy of device info for easy access.

    IPosDet  *piPosDet;
    AEECallback cbGetGPSInfo;
    AEEGPSConfig gpsConfig;
    AEEGPSInfo gpsInfo;

    int gpsRespCnt;
    int gpsReqCnt; // to track how many GPS requests are sent
    boolean bWaitingForResp;

    CSettings gpsSettings;
} PosDetApp;

/*-----------------------------------------------------------------------------
  Function Prototypes
  ----------------------------------------------------------------------------*/
boolean PosDetApp_InitAppData(PosDetApp *pMe);
void PosDetApp_FreeAppData(PosDetApp *pMe);

static boolean PosDetApp_HandleEvent(PosDetApp *pMe, AEEEvent eCode,
                                     uint16 wParam, uint32 dwParam);
static boolean PosDetApp_Start(PosDetApp *pMe);
static void PosDetApp_Stop(PosDetApp *pMe);

static void PosDetApp_Printf(PosDetApp *pMe, int nLine, int nCol, AEEFont fnt,
                             uint32 dwFlags, const char *szFormat, ...);

static uint32 PosDetApp_InitGPSSettings(PosDetApp *pMe);
static uint32 PosDetApp_ReadGPSSettings(PosDetApp *pMe, IFile *pIFile);
static uint32 PosDetApp_WriteGPSSettings(PosDetApp *pMe, IFile *pIFile);
static uint32 PosDetApp_SaveGPSSettings(PosDetApp *pMe);

/* test */
static void PosDetApp_SetGPSConfigTest(PosDetApp *pMe);
/* test */
static void PosDetApp_ShowGPSInfo(PosDetApp *pMe);
static void PosDetApp_CBGetGPSInfo_0(void *pd);
static void PosDetApp_CBGetGPSInfo_1(void *pd);
static boolean PosDetApp_SingleRequest(PosDetApp *pMe);
static boolean PosDetApp_MultipleRequests(PosDetApp *pMe);

/*-----------------------------------------------------------------------------
  Function Definitions
  ----------------------------------------------------------------------------*/

int
AEEClsCreateInstance(AEECLSID ClsId, IShell * piShell, IModule * piModule,
                     void ** ppObj)
{
    *ppObj = NULL;

    // Confirm this applet is the one intended to be created (classID matches):
    if( AEECLSID_CPOSDETAPP == ClsId ) {
        // Create the applet and make room for the applet structure.
        // NOTE: FreeAppData is called after EVT_APP_STOP is sent to
        //       HandleEvent.
        if(TRUE == AEEApplet_New(sizeof(PosDetApp),
                                 ClsId,
                                 piShell,
                                 piModule,
                                 (IApplet**)ppObj,
                                 (AEEHANDLER)PosDetApp_HandleEvent,
                                 (PFNFREEAPPDATA)PosDetApp_FreeAppData)) {

            // Initialize applet data. This is called before EVT_APP_START is
            // sent to the HandleEvent function.
            if(TRUE == PosDetApp_InitAppData((PosDetApp*)*ppObj)) {
                return AEE_SUCCESS; //Data initialized successfully.
            }
            else {
                // Release the applet. This will free the memory
                // allocated for the applet when AEEApplet_New was
                // called.
                IAPPLET_Release((IApplet*)*ppObj);
                return AEE_EFAILED;
            }
        } // End AEEApplet_New
    }
    return AEE_EFAILED;
}

boolean
PosDetApp_InitAppData(PosDetApp * pMe)
{
    // Get the device information for this handset.
    // Reference all the data by looking at the pMe->deviceInfo structure.
    // Check the API reference guide for all the handy device info you can get.
    pMe->deviceInfo.wStructSize = sizeof(pMe->deviceInfo);
    ISHELL_GetDeviceInfo(pMe->applet.m_pIShell,&pMe->deviceInfo);

    if (ISHELL_CreateInstance(pMe->applet.m_pIShell, AEECLSID_POSDET,
                              (void**)&pMe->piPosDet)
        != SUCCESS) {

        return FALSE;
    }

    return TRUE;// No failures up to this point, so return success.
}

void
PosDetApp_FreeAppData(PosDetApp * pMe)
{
    // Insert your code here for freeing any resources you have allocated...

    // Example to use for releasing each interface:
    // if ( NULL != pMe->piMenuCtl ) {     // check for NULL first
    //     IMenuCtl_Release(pMe->piMenuCtl)// release the interface
    //     pMe->piMenuCtl = NULL;          // set to NULL so no problems later
    // }
    if (pMe->piPosDet) {
        IPOSDET_Release(pMe->piPosDet);
        pMe->piPosDet = NULL;
    }
        
}

static boolean
PosDetApp_HandleEvent(PosDetApp* pMe, AEEEvent eCode,
                      uint16 wParam, uint32 dwParam)
{
    switch (eCode) {
        // Event to inform app to start, so start-up code is here:
    case EVT_APP_START:
        if (!PosDetApp_Start(pMe)) {
            PosDetApp_Printf(pMe, 1, 2, AEE_FONT_BOLD, IDF_ALIGN_CENTER|IDF_ALIGN_MIDDLE,
                             "Something goes wrong!");
            ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        }
        return TRUE;

        // Event to inform app to exit, so shut-down code is here:
    case EVT_APP_STOP:
        PosDetApp_Stop(pMe);
        return TRUE;

        // Event to inform app to suspend, so suspend code is here:
    case EVT_APP_SUSPEND:
        return TRUE;

        // Event to inform app to resume, so resume code is here:
    case EVT_APP_RESUME:
        return TRUE;

        // An SMS message has arrived for this app.
        // The Message is in the dwParam above as (char *).
        // sender simply uses this format "//BREW:ClassId:Message",
        // example //BREW:0x00000001:Hello World
    case EVT_APP_MESSAGE:
        return TRUE;

        // A key was pressed:
    case EVT_KEY:
        return FALSE;

        // Clamshell has opened/closed
        // wParam = TRUE if open, FALSE if closed
    case EVT_FLIP:
        return TRUE;

        // Clamtype device is closed and reexposed when opened, and LCD
        // is blocked, or keys are locked by software.
        // wParam = TRUE if keygaurd is on
    case EVT_KEYGUARD:
        return TRUE;

        // If event wasn't handled here, then break out:
    default:
        break;
    }
    return FALSE; // Event wasn't handled.
}

static boolean
PosDetApp_Start(PosDetApp *pMe)
{
    int ret;
    IDISPLAY_ClearScreen(pMe->applet.m_pIDisplay);

    // Read/Write GPS settings from/to the configuration file.
    ret = (int)PosDetApp_InitGPSSettings(pMe);
    if (ret != SUCCESS) {
        return FALSE;
    }

    // Get/Set GPS configuration.
    ret = IPOSDET_GetGPSConfig(pMe->piPosDet, &pMe->gpsConfig);
    if (ret != SUCCESS) {
        DBGPRINTF("GetGPSConfig err = %d", ret);
        return FALSE;
    }

    pMe->gpsConfig.mode = AEEGPS_MODE_DEFAULT;
    ret = IPOSDET_SetGPSConfig(pMe->piPosDet, &pMe->gpsConfig);
    if (ret != SUCCESS) {
        DBGPRINTF("SetGPSConfig err = %d", ret);
        return FALSE;
    }

    // Request a fix.
    /* TODO: We can choose MultipleRequests or SingleRequest according to the
       settings in the configuration file. */
    if (pMe->gpsSettings.reqType == MULTIPLE_REQUESTS) {
        PosDetApp_SetGPSConfigTest(pMe);
        return PosDetApp_MultipleRequests(pMe);
    }
    else if (pMe->gpsSettings.reqType == SINGLE_REQUEST) {
        return PosDetApp_SingleRequest(pMe);
    }
    else {
        return TRUE;
    }
}

static void
PosDetApp_Stop(PosDetApp *pMe)
{
    ISHELL_CancelTimer(pMe->applet.m_pIShell, PosDetApp_MultipleRequests, pMe);
    CALLBACK_Cancel(&pMe->cbGetGPSInfo);
}

static uint32
PosDetApp_InitGPSSettings(PosDetApp *pMe)
{
    IFileMgr *pIFileMgr = NULL;
    IFile *pIConfigFile = NULL;
    uint32 nResult = 0;

    pMe->gpsSettings.reqType = MULTIPLE_REQUESTS;

    nResult = ISHELL_CreateInstance(pMe->applet.m_pIShell, AEECLSID_FILEMGR,
                                    (void**)&pIFileMgr);
    if (SUCCESS != nResult) {
        DBGPRINTF("Failed to create IFileMgr Interface");
        return nResult;
    }

    // If the config file exists, open it and read the settings.  Otherwise, we
    // need to create a new config file.
    pIConfigFile = IFILEMGR_OpenFile(pIFileMgr, SPD_CONFIG_FILE, _OFM_READ);
    if (NULL == pIConfigFile) {
        nResult = EFAILED;
        DBGPRINTF("Failed to Open File " SPD_CONFIG_FILE);
        pIConfigFile = IFILEMGR_OpenFile(pIFileMgr, SPD_CONFIG_FILE,
                                         _OFM_CREATE);
        if (NULL == pIConfigFile) {
            int err = IFILEMGR_GetLastError(pIFileMgr);
            DBGPRINTF("Failed to Create File " SPD_CONFIG_FILE " err = %d",
                      err);
            nResult = EFAILED;
        }
        else {
            pMe->gpsSettings.optim  = AEEGPS_OPT_DEFAULT;
            pMe->gpsSettings.qos    = SPD_QOS_DEFAULT;
            pMe->gpsSettings.server.svrType = AEEGPS_SERVER_DEFAULT;
            nResult = PosDetApp_WriteGPSSettings(pMe, pIConfigFile);
        }
    }
    else {
        nResult = PosDetApp_ReadGPSSettings(pMe, pIConfigFile);
    }

    // Free the IFileMgr and IFile instances
    if (pIConfigFile) {
        (void)IFILE_Release(pIConfigFile);
    }

    (void)IFILEMGR_Release(pIFileMgr);

    return nResult;
}

uint32
PosDetApp_ReadGPSSettings(PosDetApp *pMe, IFile *pIFile)
{
    char *pBuf = NULL;
    char *pszTok = NULL;
    char *pszSvr = NULL;
    char *pszDelimiter = ";";
    int32 nResult = 0;
    AEEFileInfo fileInfo;
    int nRead = 0;

    (void)IFILE_GetInfo(pIFile, &fileInfo);
    // Allocate enough memory to read the full text into memory
    pBuf = MALLOC(fileInfo.dwSize);

    nRead = IFILE_Read(pIFile, (void*)pBuf, fileInfo.dwSize);
    if ((uint32)nRead != fileInfo.dwSize) {
        FREEIF(pBuf);
        return EFAILED;
    }

    // Check for an optimization mode setting in the file:
    pszTok = STRSTR(pBuf, SPD_CONFIG_OPT_STRING);
    if (pszTok) {
        pszTok = pszTok + STRLEN(SPD_CONFIG_OPT_STRING);
        pMe->gpsSettings.optim = (AEEGPSOpt)STRTOUL(pszTok, &pszDelimiter, 10);
    }

    // Check for a QoS setting in the file:
    pszTok = STRSTR(pBuf, SPD_CONFIG_QOS_STRING);
    if (pszTok) {
        pszTok = pszTok + STRLEN(SPD_CONFIG_QOS_STRING);
        pMe->gpsSettings.qos = (AEEGPSQos)STRTOUL(pszTok, &pszDelimiter, 10);
    }

    // Check for a server type setting in the file:
    pszTok = STRSTR(pBuf, SPD_CONFIG_SVR_TYPE_STRING);
    if (pszTok) {
        pszTok = pszTok + STRLEN(SPD_CONFIG_SVR_TYPE_STRING);
        pMe->gpsSettings.server.svrType = STRTOUL(pszTok, &pszDelimiter, 10);

        // If the server type is IP, we need to find the ip address and the
        // port number
        if (AEEGPS_SERVER_IP == pMe->gpsSettings.server.svrType) {
            pszTok = STRSTR(pBuf, SPD_CONFIG_SVR_IP_STRING);
            if (pszTok) {
                pszTok = pszTok + STRLEN(SPD_CONFIG_SVR_IP_STRING);
                nResult = DistToSemi(pszTok);
                pszSvr = MALLOC(nResult+1);
                STRLCPY(pszSvr, pszTok, nResult);
                *(pszSvr+nResult) = 0;  // Need to manually NULL-terminate the
                                        // string
                if (!INET_ATON(pszSvr,
                               &pMe->gpsSettings.server.svr.ipsvr.addr)) {
                    FREE(pszSvr);
                    FREEIF(pBuf);
                    return EFAILED;
                }
                FREE(pszSvr);
            }
            pszTok = STRSTR(pBuf, SPD_CONFIG_SVR_PORT_STRING);
            if (pszTok) {
                INPort temp;
                pszTok = pszTok + STRLEN(SPD_CONFIG_SVR_PORT_STRING);
                temp = (INPort)STRTOUL(pszTok, &pszDelimiter, 10);
                pMe->gpsSettings.server.svr.ipsvr.port = AEE_htons(temp);
            }
        }
    }

    FREEIF(pBuf);

    return SUCCESS;
}
uint32
PosDetApp_WriteGPSSettings(PosDetApp *pMe, IFile *pIFile)
{
    char *pszBuf;
    int32 nResult;
    int nWritten = 0;

    pszBuf = MALLOC(1024);

    // Truncate the file, in case it already contains data
    (void)IFILE_Truncate(pIFile, 0);

    // Write out the optimization setting:
    SNPRINTF(pszBuf, 1024, SPD_CONFIG_OPT_STRING"%d;\r\n",
             pMe->gpsSettings.optim);
    nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
    if (0 == nResult) {
        FREE(pszBuf);
        return EFAILED;
    }

    // Write out the QoS setting:
    SNPRINTF(pszBuf, 1024, SPD_CONFIG_QOS_STRING"%d;\r\n",
             pMe->gpsSettings.qos);
    nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
    if (0 == nResult) {
        FREE(pszBuf);
        return EFAILED;
    }

    // Write out the server type setting:
    SNPRINTF(pszBuf, 1024, SPD_CONFIG_SVR_TYPE_STRING"%d;\r\n",
             pMe->gpsSettings.server.svrType);
    nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
    if (0 == nResult) {
        FREE(pszBuf);
        return EFAILED;
    }

    if (AEEGPS_SERVER_IP == pMe->gpsSettings.server.svrType) {
        // Write out the IP address setting:
        INET_NTOA(pMe->gpsSettings.server.svr.ipsvr.addr, pszBuf, 50);
        nResult = IFILE_Write(pIFile, (const void*)SPD_CONFIG_SVR_IP_STRING,
                              STRLEN(SPD_CONFIG_SVR_IP_STRING));
        if (0 == nResult)
        {
            FREE(pszBuf);
            return EFAILED;
        }
        nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
        if (0 == nResult)
        {
            FREE(pszBuf);
            return EFAILED;
        }
        nResult = IFILE_Write(pIFile, (const void*)";\r\n", STRLEN(";\r\n"));
        if (0 == nResult)
        {
            FREE(pszBuf);
            return EFAILED;
        }

        // Write out the port setting:
        SNPRINTF(pszBuf, 1024, SPD_CONFIG_SVR_PORT_STRING"%d;\r\n",
                 AEE_ntohs(pMe->gpsSettings.server.svr.ipsvr.port));
        nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
        if (0 == nResult)
        {
            FREE(pszBuf);
            return EFAILED;
        }
    }

    FREE(pszBuf);

    return SUCCESS;
}

uint32
PosDetApp_SaveGPSSettings(PosDetApp *pMe )
{
    IFileMgr *pIFileMgr = NULL;
    IFile *pIConfigFile = NULL;
    uint32 nResult = 0;

    // Create the instance of IFileMgr
    nResult = ISHELL_CreateInstance(pMe->applet.m_pIShell, AEECLSID_FILEMGR,
                                    (void**)&pIFileMgr);
    if (SUCCESS != nResult) {
        return nResult;
    }

    // If the config file exists, open it and read the settings.  Otherwise, we
    // need to create a new config file.
    pIConfigFile = IFILEMGR_OpenFile(pIFileMgr, SPD_CONFIG_FILE,
                                     _OFM_READWRITE);
    if (NULL == pIConfigFile) {
        nResult = IFILEMGR_GetLastError(pIFileMgr);
        if (nResult == EFILENOEXISTS) {
            pIConfigFile = IFILEMGR_OpenFile(pIFileMgr, SPD_CONFIG_FILE,
                                             _OFM_CREATE);
            if (NULL == pIConfigFile) {
                nResult = IFILEMGR_GetLastError(pIFileMgr);
                DBGPRINTF("File Create Error %d", nResult);
                IFILEMGR_Release(pIFileMgr);
                return nResult;
            }
            nResult = PosDetApp_WriteGPSSettings( pMe, pIConfigFile );
        }
        else {
            DBGPRINTF("File Open Error %d", nResult);
            IFILEMGR_Release(pIFileMgr);
            return nResult;
        }
    }
    else {
        nResult = PosDetApp_WriteGPSSettings(pMe, pIConfigFile);
    }

    // Free the IFileMgr and IFile instances
    if (pIConfigFile) {
        (void)IFILE_Release(pIConfigFile);
    }

    IFILEMGR_Release(pIFileMgr);

    return nResult;
}

static void
PosDetApp_Printf(PosDetApp *pMe, int nLine, int nCol, AEEFont fnt,
                 uint32 dwFlags, const char *szFormat, ...)
{
    char szBuf[64];
    va_list args;
    va_start(args, szFormat);
    VSNPRINTF(szBuf, 64, szFormat, args);
    va_end(args);
    xDisplay((AEEApplet*)pMe, nLine, nCol, fnt, dwFlags, szBuf);
}

static void
PosDetApp_SetGPSConfigTest(PosDetApp *pMe)
{
    int err;
    MEMSET(&pMe->gpsConfig, 0, sizeof(AEEGPSConfig));

    // Retrieve the current configuration.
    err = IPOSDET_GetGPSConfig(pMe->piPosDet, &pMe->gpsConfig);
    if (err != SUCCESS){
        DBGPRINTF("PosDetApp: Failed to retrieve config. err = %d", err);
    }

    pMe->gpsConfig.mode = AEEGPS_MODE_TRACK_NETWORK;
    pMe->gpsConfig.nFixes = 0;
    pMe->gpsConfig.nInterval = GPSCBACK_INTERVAL;
    pMe->gpsConfig.optim = AEEGPS_OPT_SPEED;
    pMe->gpsConfig.qos = pMe->gpsSettings.qos;
    pMe->gpsConfig.server.svrType = AEEGPS_SERVER_DEFAULT;

    err = IPOSDET_SetGPSConfig(pMe->piPosDet, &pMe->gpsConfig);

    if (err != SUCCESS) {
        DBGPRINTF("PosDetApp: Configuration could not be set! err = %d", err);
    }
}

static void
PosDetApp_CBGetGPSInfo_0(void *pd)
{
    PosDetApp *pMe = (PosDetApp*)pd;
    if(pMe->gpsInfo.status == AEEGPS_ERR_NO_ERR) {
        PosDetApp_ShowGPSInfo(pMe);
    }
    else {
        PosDetApp_Printf(pMe, 1, 2, AEE_FONT_BOLD, IDF_ALIGN_CENTER,
                         "error: GetGPSInfo status = %u",
                         pMe->gpsInfo.status);
    }
}

static void
PosDetApp_CBGetGPSInfo_1(void *pd)
{
    PosDetApp *pMe = (PosDetApp*)pd;
    pMe->gpsRespCnt++;

    pMe->bWaitingForResp = FALSE;
    if (pMe->gpsInfo.status == AEEGPS_ERR_NO_ERR
        || (pMe->gpsInfo.status == AEEGPS_ERR_INFO_UNAVAIL
            && pMe->gpsInfo.fValid)) {
        PosDetApp_ShowGPSInfo(pMe);
        ISHELL_SetTimer(pMe->applet.m_pIShell, GPSCBACK_INTERVAL * 1000,
                        PosDetApp_MultipleRequests, pMe);
    }
    else {
        PosDetApp_Printf(pMe, 1, 2, AEE_FONT_BOLD, IDF_ALIGN_CENTER,
                         "error: GetGPSInfo status = %u",
                         pMe->gpsInfo.status);
        return;
    }
}

static boolean
PosDetApp_SingleRequest(PosDetApp *pMe)
{
    CALLBACK_Init(&pMe->cbGetGPSInfo, PosDetApp_CBGetGPSInfo_0, (void*)pMe);
    if (IPOSDET_GetGPSInfo(pMe->piPosDet,
                           AEEGPS_GETINFO_LOCATION | AEEGPS_GETINFO_ALTITUDE
                           | AEEGPS_GETINFO_VELOCITY,
                           AEEGPS_ACCURACY_LEVEL6,
                           &pMe->gpsInfo, &pMe->cbGetGPSInfo)
        != SUCCESS) {
        //return failure
        return FALSE;
    }
    return TRUE;
}

static boolean
PosDetApp_MultipleRequests(PosDetApp *pMe)
{
    int ret = 0;

    if (pMe->bWaitingForResp) {
        return TRUE;
    }

    CALLBACK_Cancel(&pMe->cbGetGPSInfo);
    CALLBACK_Init(&pMe->cbGetGPSInfo, PosDetApp_CBGetGPSInfo_1, (void*)pMe);
    ret = IPOSDET_GetGPSInfo(pMe->piPosDet,
                             AEEGPS_GETINFO_LOCATION | AEEGPS_GETINFO_ALTITUDE
                             | AEEGPS_GETINFO_VELOCITY,
                             AEEGPS_ACCURACY_LEVEL1,
                             &pMe->gpsInfo, &pMe->cbGetGPSInfo);
    if (SUCCESS == ret) {
        // continue
        pMe->bWaitingForResp = TRUE;
    }
    else if (EUNSUPPORTED == ret) {
        DBGPRINTF("PosDetApp: GetGPSInfo request isn't supported.");
        return FALSE;
    }
    else {
        DBGPRINTF("PosDetApp: GetGPSInfo failed: err = %d", ret);
        return FALSE;
    }

    pMe->gpsReqCnt++;
    PosDetApp_Printf(pMe, 0, 2, AEE_FONT_BOLD,
                     IDF_ALIGN_LEFT | IDF_RECT_FILL,
                     "req : %d", pMe->gpsReqCnt);

    return TRUE;
}

static void
PosDetApp_ShowGPSInfo(PosDetApp *pMe)
{
    AEEPositionInfoEx posInfo;
    int line = 0;
    JulianType jd;

#define MAXTEXTLEN   22
    AECHAR wcText[MAXTEXTLEN];
    char szStr[MAXTEXTLEN];

    ZEROAT(&posInfo);
    posInfo.dwSize = sizeof(AEEPositionInfoEx);
    IPOSDET_ExtractPositionInfo(pMe->piPosDet, &pMe->gpsInfo, &posInfo);

    IDISPLAY_ClearScreen(pMe->applet.m_pIDisplay);

    PosDetApp_Printf(pMe, line++, 2, AEE_FONT_BOLD, IDF_ALIGN_LEFT,
                     "req : %d", pMe->gpsReqCnt);

    PosDetApp_Printf(pMe, line++, 2, AEE_FONT_BOLD, IDF_ALIGN_LEFT,
                     "resp : %d", pMe->gpsRespCnt);

    GETJULIANDATE(pMe->gpsInfo.dwTimeStamp, &jd);
    PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                     "Time = %d %d/%d %d:%d:%d", jd.wYear, jd.wMonth, jd.wDay,
                     jd.wHour, jd.wMinute, jd.wSecond);
    PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                     "dwSize = %d bytes", posInfo.dwSize);
    if (posInfo.fLatitude) {
        (void)FLOATTOWSTR(posInfo.Latitude, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Latitude = %s d", szStr);
    }
    if (posInfo.fLongitude) {
        (void)FLOATTOWSTR(posInfo.Longitude, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Longitude = %s d", szStr);
    }
    if (posInfo.fAltitude) {
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Altitude = %d m", posInfo.nAltitude);
    }
    if (posInfo.fHeading) {
        (void)FLOATTOWSTR(posInfo.Heading, wcText, MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Heading = %s d", szStr);
    }
    if (posInfo.fHorVelocity) {
        (void)FLOATTOWSTR(posInfo.HorVelocity, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "HorVelocity = %s m/s", szStr);
    }
    if (posInfo.fVerVelocity) {
        (void)FLOATTOWSTR(posInfo.VerVelocity, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "VerVelocity = %s m/s", szStr);
    }
}
