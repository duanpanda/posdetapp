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
#include "AEESockPort.h"

#include "CPosDetApp.h"
#include "RyanUtils.h"
#include "PosDetApp_res.h"

#define NO_USER_CONFIG -1

typedef uint8 RequestType;

enum {
    SINGLE_REQUEST,
    MULTIPLE_REQUESTS
};

typedef struct _CSettings
{
    AEEGPSServer server;
    AEEGPSOpt    optim;
    AEEGPSQos    qos;
    RequestType  reqType;
    uint8        reserved;
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

typedef struct _UploadSvr {
    INAddr addr;
    INPort port;
    uint16 reserved;
} UploadSvr;

typedef struct _PosDetApp {
    AEEApplet           applet;
    AEEDeviceInfo       deviceInfo;
    IPosDet            *pIPosDet;
    IFileMgr           *pIFileMgr;
    IFile              *pLogFile;
    ISockPort          *pISockPort;
    AEECallback         cbSendTo;
    AEESockAddrStorage  sockAddr;
    uint32              uBytesSent;
    AEECallback         cbGetGPSInfo;
    AEECallback         cbReqInterval;
    AEEGPSConfig        gpsConfig;
    AEEGPSInfo          gpsInfo;
    AEEPositionInfoEx   posInfoEx;
    CSettings           gpsSettings;
    char                reportStr[REPORT_STR_BUF_SIZE];
    UploadSvr           uploadSvr;
    int                 gpsRespCnt;
    int                 gpsReqCnt; // to track how many GPS requests are sent
    int                 tcpTryCnt;
    int                 tcpConnMaxTry; // max times to try connecting to server
    boolean             bWaitingForResp;
    boolean             bConnected; // is connected to server
    boolean             bSending;
    boolean             bSendSucceeds;
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

/* test */
static void PosDetApp_Printf(PosDetApp *pMe, int nLine, int nCol, AEEFont fnt,
                             uint32 dwFlags, const char *szFormat, ...);

static uint32 PosDetApp_InitGPSSettings(PosDetApp *pMe);
static uint32 PosDetApp_ReadGPSSettings(PosDetApp *pMe, IFile *pIFile);
//static uint32 PosDetApp_WriteGPSSettings(PosDetApp *pMe, IFile *pIFile);
//static uint32 PosDetApp_SaveGPSSettings(PosDetApp *pMe);
static int PosDetApp_DecodePosInfo(PosDetApp *pMe);
static void PosDetApp_MakeReportStr(PosDetApp *pMe);
static boolean PosDetApp_StartTCPClient(PosDetApp *pMe);
static void PosDetApp_TryConnect(void *po);
static void PosDetApp_TryWriteToSvr(void *po);
static void PosDetApp_ProcessGPSData(PosDetApp *pMe);
static boolean PosDetApp_RequestAFix(PosDetApp *pMe);

/* test */
static int PosDetApp_GetLogFile(PosDetApp *pMe);
static void PosDetApp_LogPos(PosDetApp *pMe);

/* test */
static void PosDetApp_CnfgTrackNetwork(PosDetApp *pMe);
/* test */
static void PosDetApp_ShowGPSInfo(PosDetApp *pMe);

static void PosDetApp_CBGetGPSInfo_SingleReq(void *pd);
static void PosDetApp_CBGetGPSInfo_MultiReq(void *pd);
static boolean PosDetApp_SingleRequest(PosDetApp *pMe);
static boolean PosDetApp_MultipleRequests(PosDetApp *pMe);

/* user config file support */
static int PosDetApp_ReadUserConfig(PosDetApp *pMe);
static void PosDetApp_ApplyDefaultConfig(PosDetApp *pMe);

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
    int err = 0;

    /* Get device info. */
    pMe->deviceInfo.wStructSize = sizeof(pMe->deviceInfo);
    ISHELL_GetDeviceInfo(pMe->applet.m_pIShell,&pMe->deviceInfo);

    /* Create IPosDet. */
    err = ISHELL_CreateInstance(pMe->applet.m_pIShell, AEECLSID_POSDET,
        (void**)&pMe->pIPosDet);
    if (err != SUCCESS) {
        DBGPRINTF("Failed to create IPosDet Interface");
        return FALSE;
    }

    /* Create IFileMgr. */
    err = ISHELL_CreateInstance(pMe->applet.m_pIShell, AEECLSID_FILEMGR,
        (void**)&pMe->pIFileMgr);
    if (err != SUCCESS) {
        DBGPRINTF("Failed to create IFileMgr Interface");
        return FALSE;
    }

    /* Clear report string buffer. */
    MEMSET(pMe->reportStr, 0, REPORT_STR_BUF_SIZE);

    /* Load default config */
    PosDetApp_ApplyDefaultConfig(pMe);
    /* Get user config. */
    err = PosDetApp_ReadUserConfig(pMe);
    if (SUCCESS != err && NO_USER_CONFIG != err) {
        DBGPRINTF("Error read config file: err=%d");
        return FALSE;
    }

    pMe->bWaitingForResp = FALSE;
    pMe->bConnected = FALSE;
    pMe->uBytesSent = 0;
    pMe->bSending = FALSE;
    pMe->bSendSucceeds = FALSE;
    pMe->tcpTryCnt = 0;

    /* Create ISockPort. */
    if (!PosDetApp_StartTCPClient(pMe)) {
        return FALSE;
    }

    /* test: Create log file. */
    err = PosDetApp_GetLogFile(pMe);
    if (err != SUCCESS) {
        DBGPRINTF("Failed to create file " SPD_LOG_FILE " err = %d", err);
        return FALSE;
    }

    return TRUE;
}

void
PosDetApp_FreeAppData(PosDetApp * pMe)
{
    IQI_RELEASEIF(pMe->pLogFile);
    CALLBACK_Cancel(&pMe->cbSendTo);
    IQI_RELEASEIF(pMe->pISockPort);
    IQI_RELEASEIF(pMe->pIFileMgr);
    IQI_RELEASEIF(pMe->pIPosDet);
}

static boolean
PosDetApp_HandleEvent(PosDetApp* pMe, AEEEvent eCode,
                      uint16 wParam, uint32 dwParam)
{
    switch (eCode) {
        // Event to inform app to start, so start-up code is here:
    case EVT_APP_START:
        DBGPRINTF("******** EVT_APP_START");
        if (!PosDetApp_Start(pMe)) {
            PosDetApp_Printf(pMe, 1, 2, AEE_FONT_BOLD,
                             IDF_ALIGN_CENTER | IDF_ALIGN_MIDDLE,
                             "Something goes wrong!");
            ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        }
        return TRUE;
    case EVT_APP_START_BACKGROUND:
        DBGPRINTF("******** EVT_APP_START_BACKGROUND");
        if (!PosDetApp_Start(pMe)) {
            PosDetApp_Printf(pMe, 1, 2, AEE_FONT_BOLD,
                IDF_ALIGN_CENTER | IDF_ALIGN_MIDDLE,
                "Something goes wrong!");
            ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        }
        return TRUE;

        // Event to inform app to exit, so shut-down code is here:
    case EVT_APP_STOP:
        //{
        //    boolean *bToBackground = (boolean*)dwParam;
        //    *bToBackground = FALSE;
        //    ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        //}
        DBGPRINTF("******** EVT_APP_STOP");
        PosDetApp_Stop(pMe);
        return TRUE;

        // Event to inform app to suspend, so suspend code is here:
    case EVT_APP_SUSPEND:
        return TRUE;

        // Event to inform app to resume, so resume code is here:
    case EVT_APP_RESUME:
        return TRUE;


    case EVT_NOTIFY:
        {
            AEENotify *pNotify = (AEENotify*)dwParam;
            if (pNotify->cls == AEECLSID_SHELL
                && (pNotify->dwMask & NMASK_SHELL_INIT)) {

                ISHELL_StartBackgroundApplet(pMe->applet.m_pIShell,
                                             AEECLSID_CPOSDETAPP, NULL);
            }
        }
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
    ret = IPOSDET_GetGPSConfig(pMe->pIPosDet, &pMe->gpsConfig);
    if (ret != SUCCESS) {
        DBGPRINTF("GetGPSConfig err = %d", ret);
        return FALSE;
    }

    pMe->gpsConfig.mode = AEEGPS_MODE_DEFAULT;
    ret = IPOSDET_SetGPSConfig(pMe->pIPosDet, &pMe->gpsConfig);
    if (ret != SUCCESS) {
        DBGPRINTF("SetGPSConfig err = %d", ret);
        return FALSE;
    }

    (void)IFILE_Write(pMe->pLogFile, "\r\n", STRLEN("\r\n"));

    // Try connecting to the server.
    PosDetApp_TryConnect(pMe);

    // Only when connection established does it request a fix.

    return TRUE;
}

static void
PosDetApp_Stop(PosDetApp *pMe)
{
    (void)ISockPort_Close(pMe->pISockPort);
    CALLBACK_Cancel(&pMe->cbReqInterval);
    CALLBACK_Cancel(&pMe->cbGetGPSInfo);
}

static uint32
PosDetApp_InitGPSSettings(PosDetApp *pMe)
{
    IFile *pCnfgFile = NULL;
    uint32 nResult = 0;

    pMe->gpsSettings.reqType = MULTIPLE_REQUESTS;

    // If the config file exists, open it and read the settings.  Otherwise, we
    // need to create a new config file and write default settings in.
    pCnfgFile = IFILEMGR_OpenFile(pMe->pIFileMgr, SPD_CONFIG_FILE, _OFM_READ);
    if (NULL == pCnfgFile) {
        nResult = EFAILED;
        DBGPRINTF("Failed to Open File " SPD_CONFIG_FILE);
        pCnfgFile = IFILEMGR_OpenFile(pMe->pIFileMgr, SPD_CONFIG_FILE,
                                      _OFM_CREATE);
        if (NULL == pCnfgFile) {
            int err = IFILEMGR_GetLastError(pMe->pIFileMgr);
            DBGPRINTF("Failed to Create File " SPD_CONFIG_FILE " err = %d",
                      err);
            nResult = EFAILED;
        }
        else {
            pMe->gpsSettings.optim = AEEGPS_OPT_DEFAULT;
            pMe->gpsSettings.qos = SPD_QOS_DEFAULT;
            pMe->gpsSettings.server.svrType = AEEGPS_SERVER_DEFAULT;
            //nResult = PosDetApp_WriteGPSSettings(pMe, pCnfgFile);
        }
    }
    else {
        nResult = PosDetApp_ReadGPSSettings(pMe, pCnfgFile);
    }

    // Free the IFileMgr and IFile instances
    if (pCnfgFile) {
        (void)IFILE_Release(pCnfgFile);
    }

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
    if (NULL == pBuf) {
        return ENOMEMORY;
    }

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
                STRLCPY(pszSvr, pszTok, nResult + 1);
                if (!INET_PTON(AEE_AF_INET, pszSvr,
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

//uint32
//PosDetApp_WriteGPSSettings(PosDetApp *pMe, IFile *pIFile)
//{
//    char *pszBuf = NULL;
//    int32 nResult = 0;
//
//    pszBuf = MALLOC(1024);
//    if (NULL == pszBuf) {
//        return ENOMEMORY;
//    }
//
//    // Truncate the file, in case it already contains data
//    (void)IFILE_Truncate(pIFile, 0);
//
//    // Write out the optimization setting:
//    SNPRINTF(pszBuf, 1024, SPD_CONFIG_OPT_STRING"%d;\r\n",
//             pMe->gpsSettings.optim);
//    nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
//    if (0 == nResult) {
//        FREE(pszBuf);
//        return EFAILED;
//    }
//
//    // Write out the QoS setting:
//    SNPRINTF(pszBuf, 1024, SPD_CONFIG_QOS_STRING"%d;\r\n",
//             pMe->gpsSettings.qos);
//    nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
//    if (0 == nResult) {
//        FREE(pszBuf);
//        return EFAILED;
//    }
//
//    // Write out the server type setting:
//    SNPRINTF(pszBuf, 1024, SPD_CONFIG_SVR_TYPE_STRING"%d;\r\n",
//             pMe->gpsSettings.server.svrType);
//    nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
//    if (0 == nResult) {
//        FREE(pszBuf);
//        return EFAILED;
//    }
//
//    if (AEEGPS_SERVER_IP == pMe->gpsSettings.server.svrType) {
//        // Write out the IP address setting:
//        INET_NTOA(pMe->gpsSettings.server.svr.ipsvr.addr, pszBuf, 50);
//        nResult = IFILE_Write(pIFile, (const void*)SPD_CONFIG_SVR_IP_STRING,
//                              STRLEN(SPD_CONFIG_SVR_IP_STRING));
//        if (0 == nResult)
//        {
//            FREE(pszBuf);
//            return EFAILED;
//        }
//        nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
//        if (0 == nResult)
//        {
//            FREE(pszBuf);
//            return EFAILED;
//        }
//        nResult = IFILE_Write(pIFile, (const void*)";\r\n", STRLEN(";\r\n"));
//        if (0 == nResult)
//        {
//            FREE(pszBuf);
//            return EFAILED;
//        }
//
//        // Write out the port setting:
//        SNPRINTF(pszBuf, 1024, SPD_CONFIG_SVR_PORT_STRING"%d;\r\n",
//                 AEE_ntohs(pMe->gpsSettings.server.svr.ipsvr.port));
//        nResult = IFILE_Write(pIFile, (const void*)pszBuf, STRLEN(pszBuf));
//        if (0 == nResult)
//        {
//            FREE(pszBuf);
//            return EFAILED;
//        }
//    }
//
//    FREE(pszBuf);
//
//    return SUCCESS;
//}

//uint32
//PosDetApp_SaveGPSSettings(PosDetApp *pMe )
//{
//    IFile *pCnfgFile = NULL;
//    uint32 nResult = 0;
//
//    // If the config file exists, open it and read the settings.  Otherwise, we
//    // need to create a new config file.
//    pCnfgFile = IFILEMGR_OpenFile(pMe->pIFileMgr, SPD_CONFIG_FILE,
//                                     _OFM_READWRITE);
//    if (NULL == pCnfgFile) {
//        nResult = IFILEMGR_GetLastError(pMe->pIFileMgr);
//        if (nResult == EFILENOEXISTS) {
//            pCnfgFile = IFILEMGR_OpenFile(pMe->pIFileMgr, SPD_CONFIG_FILE,
//                                             _OFM_CREATE);
//            if (NULL == pCnfgFile) {
//                nResult = IFILEMGR_GetLastError(pMe->pIFileMgr);
//                DBGPRINTF("File Create Error %d", nResult);
//                IFILEMGR_Release(pMe->pIFileMgr);
//                return nResult;
//            }
//            nResult = PosDetApp_WriteGPSSettings( pMe, pCnfgFile );
//        }
//        else {
//            DBGPRINTF("File Open Error %d", nResult);
//            return nResult;
//        }
//    }
//    else {
//        nResult = PosDetApp_WriteGPSSettings(pMe, pCnfgFile);
//    }
//
//    // Free the IFileMgr and IFile instances
//    if (pCnfgFile) {
//        (void)IFILE_Release(pCnfgFile);
//    }
//
//    return nResult;
//}

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
PosDetApp_CnfgTrackNetwork(PosDetApp *pMe)
{
    int err;
    MEMSET(&pMe->gpsConfig, 0, sizeof(AEEGPSConfig));

    // Retrieve the current configuration.
    err = IPOSDET_GetGPSConfig(pMe->pIPosDet, &pMe->gpsConfig);
    if (err != SUCCESS){
        DBGPRINTF("PosDetApp: Failed to retrieve config. err = %d", err);
    }

    pMe->gpsConfig.mode = AEEGPS_MODE_TRACK_NETWORK;
    pMe->gpsConfig.nFixes = 0;
    pMe->gpsConfig.nInterval = GPSCBACK_INTERVAL;
    pMe->gpsConfig.optim = AEEGPS_OPT_SPEED;
    pMe->gpsConfig.qos = pMe->gpsSettings.qos;
    pMe->gpsConfig.server.svrType = AEEGPS_SERVER_DEFAULT;

    err = IPOSDET_SetGPSConfig(pMe->pIPosDet, &pMe->gpsConfig);

    if (err != SUCCESS) {
        DBGPRINTF("PosDetApp: Configuration could not be set! err = %d", err);
    }
}

static void
PosDetApp_CBGetGPSInfo_SingleReq(void *pd)
{
    PosDetApp *pMe = (PosDetApp*)pd;
    if(pMe->gpsInfo.status == AEEGPS_ERR_NO_ERR) {
        PosDetApp_ProcessGPSData(pMe);
    }
    else {
        DBGPRINTF("GetGPSInfo err = 0x%x",
                  pMe->gpsInfo.status);
        PosDetApp_Printf(pMe, 1, 2, AEE_FONT_BOLD, IDF_ALIGN_CENTER,
                         "GetGPSInfo err=0x%x",
                         pMe->gpsInfo.status);
    }
}

static void
PosDetApp_CBGetGPSInfo_MultiReq(void *pd)
{
    PosDetApp *pMe = (PosDetApp*)pd;
    pMe->gpsRespCnt++;

    pMe->bWaitingForResp = FALSE;
    if (pMe->gpsInfo.status == AEEGPS_ERR_NO_ERR
        || (pMe->gpsInfo.status == AEEGPS_ERR_INFO_UNAVAIL
            && pMe->gpsInfo.fValid)) {

        PosDetApp_ProcessGPSData(pMe);
        /* Initiate next request for GPS fix. */
        ISHELL_SetTimerEx(pMe->applet.m_pIShell, 0,
                          &pMe->cbReqInterval);
    }
    else {
        DBGPRINTF("GetGPSInfo err = 0x%x",
                  pMe->gpsInfo.status);
        PosDetApp_Printf(pMe, 1, 2, AEE_FONT_BOLD, IDF_ALIGN_CENTER,
                         "GetGPSInfo err=0x%x",
                         pMe->gpsInfo.status);
    }
}

static boolean
PosDetApp_SingleRequest(PosDetApp *pMe)
{
    CALLBACK_Cancel(&pMe->cbGetGPSInfo);
    CALLBACK_Init(&pMe->cbGetGPSInfo, PosDetApp_CBGetGPSInfo_SingleReq,
                  (void*)pMe);
    if (IPOSDET_GetGPSInfo(pMe->pIPosDet,
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
    CALLBACK_Init(&pMe->cbGetGPSInfo, PosDetApp_CBGetGPSInfo_MultiReq,
                  (void*)pMe);
    ret = IPOSDET_GetGPSInfo(pMe->pIPosDet,
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
    int line = 0;
    JulianType jd;

#define MAXTEXTLEN   22
    AECHAR wcText[MAXTEXTLEN];
    char szStr[MAXTEXTLEN];

    IDISPLAY_ClearScreen(pMe->applet.m_pIDisplay);

    PosDetApp_Printf(pMe, line++, 2, AEE_FONT_BOLD, IDF_ALIGN_LEFT,
                     "req : %d", pMe->gpsReqCnt);

    PosDetApp_Printf(pMe, line++, 2, AEE_FONT_BOLD, IDF_ALIGN_LEFT,
                     "resp : %d", pMe->gpsRespCnt);

    GETJULIANDATE(pMe->gpsInfo.dwTimeStamp, &jd);
    PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                     "Time = %02d-%02d-%02d %02d:%02d:%02d GMT+8", jd.wYear,
                     jd.wMonth, jd.wDay, jd.wHour + 8, jd.wMinute, jd.wSecond);
    if (pMe->posInfoEx.fLatitude) {
        (void)FLOATTOWSTR(pMe->posInfoEx.Latitude, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Latitude = %s d", szStr);
    }
    if (pMe->posInfoEx.fLongitude) {
        (void)FLOATTOWSTR(pMe->posInfoEx.Longitude, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Longitude = %s d", szStr);
    }
    if (pMe->posInfoEx.fAltitude) {
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Altitude = %d m", pMe->posInfoEx.nAltitude);
    }
    if (pMe->posInfoEx.fHeading) {
        (void)FLOATTOWSTR(pMe->posInfoEx.Heading, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "Heading = %s d", szStr);
    }
    if (pMe->posInfoEx.fHorVelocity) {
        (void)FLOATTOWSTR(pMe->posInfoEx.HorVelocity, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "HorVelocity = %s m/s", szStr);
    }
    if (pMe->posInfoEx.fVerVelocity) {
        (void)FLOATTOWSTR(pMe->posInfoEx.VerVelocity, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szStr, MAXTEXTLEN);
        PosDetApp_Printf(pMe, line++, 2, AEE_FONT_NORMAL, IDF_ALIGN_LEFT,
                         "VerVelocity = %s m/s", szStr);
    }
}


/************************************
 Returns:   int
  SUCCESS : The function succeeded.
  EPRIVLEVEL: This error is returned if the calling BREW application does not
              have PL_POS_LOCATION privilege set in the Module Information File
              (MIF). This privilege is required to invoke this function. Correct
              the MIF and try again.
  EBADPARM : This error is returned if the pi or po parameter specified are
             invalid (NULL).
  EUNSUPPORTED: This error is returned if this function is not supported by the
                device.
  EFAILED : General failure
 ************************************/
int
PosDetApp_DecodePosInfo(PosDetApp *pMe)
{
    ZEROAT(&pMe->posInfoEx);
    pMe->posInfoEx.dwSize = sizeof(AEEPositionInfoEx);
    return IPOSDET_ExtractPositionInfo(pMe->pIPosDet, &pMe->gpsInfo,
                                       &pMe->posInfoEx);
}

/* After this function, pMe->reportStr contains the whole piece of GPS data
 * to be reported to the server.*/
void
PosDetApp_MakeReportStr(PosDetApp *pMe)
{
    /* Fill the string buffer part by part, each part from the temp string. */

    char *pTmp = pMe->reportStr;         /* points to the temp buffer */
    int tmpBufSize = REPORT_STR_BUF_SIZE; /* size of temp buffer at pTmp */
#define MAXTEXTLEN 22
    AECHAR wcText[MAXTEXTLEN];  /* used to hold the float number string */
    char szTmpStr[MAXTEXTLEN];  /* snprintf this temp string to tmp buffer */
    int tmpStrLen = 0;          /* string length in bytes of the temp string */
    JulianType jd;

    /* Clear the report string buffer */
    MEMSET(pMe->reportStr, 0, REPORT_STR_BUF_SIZE);

    /* message_header + terminal_id + time */
    GETJULIANDATE(pMe->gpsInfo.dwTimeStamp, &jd);
    SNPRINTF(pTmp, tmpBufSize,
             "{EHL,A,02,%s,%02d-%02d-%02d %02d:%02d:%02d,",
             TERMINAL_ID,
             jd.wYear, jd.wMonth, jd.wDay, jd.wHour + 8, jd.wMinute,
             jd.wSecond);

    /* temp buffer head moves forward */
    tmpStrLen = STRLEN(pTmp);
    pTmp += tmpStrLen;
    tmpBufSize -= tmpStrLen;

    /* latitude */
    if (pMe->posInfoEx.fLatitude) {
        (void)FLOATTOWSTR(pMe->posInfoEx.Latitude, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szTmpStr, MAXTEXTLEN);
    }
    /* If no valid latitude value, the following code sprintf only "," */
    SNPRINTF(pTmp, tmpBufSize, "%s,", szTmpStr);
    /* Clear temp text buffers */
    MEMSET(wcText, 0, MAXTEXTLEN * sizeof(AECHAR));
    MEMSET(szTmpStr, 0, MAXTEXTLEN);

    tmpStrLen = STRLEN(pTmp);
    pTmp += tmpStrLen;
    tmpBufSize -= tmpStrLen;

    /* longitude */
    if (pMe->posInfoEx.fLongitude) {
        (void)FLOATTOWSTR(pMe->posInfoEx.Longitude, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szTmpStr, MAXTEXTLEN);
    }
    SNPRINTF(pTmp, tmpBufSize, "%s,", szTmpStr);
    MEMSET(wcText, 0, MAXTEXTLEN * sizeof(AECHAR));
    MEMSET(szTmpStr, 0, MAXTEXTLEN);

    tmpStrLen = STRLEN(pTmp);
    pTmp += tmpStrLen;
    tmpBufSize -= tmpStrLen;

    /* altitude */
    if (pMe->posInfoEx.fAltitude) {
        SNPRINTF(pTmp, tmpBufSize, "%d,", pMe->posInfoEx.nAltitude);
    }
    else {
        SNPRINTF(pTmp, tmpBufSize, ",");
    }

    tmpStrLen = STRLEN(pTmp);
    pTmp += tmpStrLen;
    tmpBufSize -= tmpStrLen;

    /* velocity */
    if (pMe->posInfoEx.fHorVelocity) {
        (void)FLOATTOWSTR(pMe->posInfoEx.HorVelocity, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szTmpStr, MAXTEXTLEN);
    }
    SNPRINTF(pTmp, tmpStrLen, "%s,", szTmpStr);
    MEMSET(wcText, 0, MAXTEXTLEN * sizeof(AECHAR));
    MEMSET(szTmpStr, 0, MAXTEXTLEN);

/*
    if (pMe->posInfoEx.fVerVelocity) {
        (void)FLOATTOWSTR(pMe->posInfoEx.VerVelocity, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szTmpStr, MAXTEXTLEN);
        SNPRINTF(pTmp, tmpBufSize, "%s,", szTmpStr)
    }
    MEMSET(wcText, 0, MAXTEXTLEN * sizeof(AECHAR));
    MEMSET(szTmpStr, 0, MAXTEXTLEN);

    tmpStrLen = STRLEN(pTmp);
    pTmp += tmpStrLen;
    tmpBufSize -= tmpStrLen;
*/

    /* heading */
    if (pMe->posInfoEx.fHeading) {
        (void)FLOATTOWSTR(pMe->posInfoEx.Heading, wcText,
                          MAXTEXTLEN * sizeof(AECHAR));
        WSTR_TO_STR(wcText, szTmpStr, MAXTEXTLEN);
    }
    SNPRINTF(pTmp, tmpBufSize, "%s,", szTmpStr);
    MEMSET(wcText, 0, MAXTEXTLEN * sizeof(AECHAR));
    MEMSET(szTmpStr, 0, MAXTEXTLEN);

    tmpStrLen = STRLEN(pTmp);
    pTmp += tmpStrLen;
    tmpBufSize -= tmpStrLen;

    /* mileage + terminal_status + terminal_alarm + satellite_num +
       policeman_id + message_tail */
    SNPRINTF(pTmp, tmpBufSize, "%s,%s,%s,%s,%s,EHL}",
             MILEAGE, TERMINAL_STATUS, TERMINAL_ALARM, SATELLITE_NUM,
             POLICEMAN_ID);
}

/* If create log file successfully, return SUCCESS, otherwise return fail
 * code. */
static int
PosDetApp_GetLogFile(PosDetApp *pMe)
{
    int err = 0;

    if (IFILEMGR_Test(pMe->pIFileMgr, SPD_LOG_FILE) != SUCCESS) {
        pMe->pLogFile = IFILEMGR_OpenFile(pMe->pIFileMgr, SPD_LOG_FILE,
                                          _OFM_CREATE);
        if (NULL == pMe->pLogFile) {
            err = IFILEMGR_GetLastError(pMe->pIFileMgr);
            return err;
        }
    }

    /* The file has existed. */
    if (NULL == pMe->pLogFile) {
        pMe->pLogFile = IFILEMGR_OpenFile(pMe->pIFileMgr, SPD_LOG_FILE,
                                          _OFM_APPEND);
        if (NULL == pMe->pLogFile) {
            err = IFILEMGR_GetLastError(pMe->pIFileMgr);
            return err;
        }
    }

    return SUCCESS;
}

static void
PosDetApp_LogPos(PosDetApp *pMe)
{
#define NEWLINE "\r\n"

    int err = 0;
    uint32 wroteBytes;

    wroteBytes = IFILE_Write(pMe->pLogFile, pMe->reportStr,
                             STRLEN(pMe->reportStr));
    if (0 == wroteBytes) {
        err = IFILEMGR_GetLastError(pMe->pIFileMgr);
        DBGPRINTF("Write log file failed: err = %d", err);
        return;
    }

    /* Write the Newline character */
    wroteBytes = IFILE_Write(pMe->pLogFile, NEWLINE, STRLEN(NEWLINE));
    if (wroteBytes != STRLEN(NEWLINE)) {
        err = IFILEMGR_GetLastError(pMe->pIFileMgr);
        DBGPRINTF("Write log file failed: err = %d", err);
    }
}

static boolean
PosDetApp_StartTCPClient(PosDetApp *pMe)
{
    int ret = 0;

    // Create the ISockPort object.
    ret = ISHELL_CreateInstance(pMe->applet.m_pIShell, AEECLSID_SockPort,
                                (void**)&(pMe->pISockPort));
    if (AEE_SUCCESS != ret) {
        DBGPRINTF("Failed to create ISockPort: err = %d", ret);
        return FALSE;
    }

    // Open the SockPort.
    ret = ISockPort_OpenEx(pMe->pISockPort,      // ISockPort pointer
        AEE_AF_INET,                             // wFamily     = IPv4
        AEE_SOCKPORT_STREAM,                     // socket type = TCP
        0                                        // Protocol type:
                                // Use 0 (recommended) to let the system
                                // select its default
                                // protocol for the given address
                                // family and socket type
        );
    if (AEE_SUCCESS != ret) {
        DBGPRINTF("Failed to open ISockPort: err = %d", ret);
        return FALSE;
    }

    return TRUE;
}

static void
PosDetApp_TryConnect(void *po)
{
    int ret;
    PosDetApp *pMe = (PosDetApp*)po;

    if (pMe->tcpTryCnt >= pMe->tcpConnMaxTry) {
        DBGPRINTF("PosDetApp: cannot connect to server after %d tries.",
                  pMe->tcpTryCnt);
        ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        return;
    }

    // connect to the distant server
    ret = ISockPort_Connect(pMe->pISockPort,  // ISockPort object
                            &pMe->sockAddr    // pointer to dest socket
                                              // address struct
        );
    pMe->tcpTryCnt++;

    if (AEEPORT_WAIT == ret) {
        ISockPort_WriteableEx(pMe->pISockPort, &pMe->cbSendTo,
                              PosDetApp_TryConnect, pMe);
        pMe->bConnected = FALSE;
        return;
    }
    else if (AEE_NET_ETIMEDOUT == ret || AEE_NET_ECONNREFUSED == ret) {
        PosDetApp_Printf(pMe, 0, 2, AEE_FONT_BOLD,
                         IDF_ALIGN_CENTER | IDF_ALIGN_MIDDLE,
                         "PosDetApp: SockPort timed out! %d Tries. Close Applet.");
        DBGPRINTF("PosDetApp: SockPort timed out! %d tries. Close Applet.",
                  pMe->tcpTryCnt);
        ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        return;
    }
    else if (AEE_SUCCESS != ret) {
        pMe->bConnected = FALSE;
        /* TODO */
        DBGPRINTF("PosDetApp: SockPort connect err = 0x%x", ret);
        return;
    }

    // (AEE_SUCCESS == ret), the SockPort is connected
    pMe->bConnected = TRUE;

    // Start a request for GPS fix.
    (void)PosDetApp_RequestAFix(pMe);
}

static void
PosDetApp_TryWriteToSvr(void *po)
{
    int ret = 0;
    PosDetApp *pMe = (PosDetApp*)po;

    // write the data to the server.
    ret = ISockPort_Write(pMe->pISockPort, // ISockPort object
                          pMe->reportStr + pMe->uBytesSent, // buffer to write
                          SOCK_BUF_SIZE - pMe->uBytesSent);  // buffer length

    // the system can't write data at the moment.
    if (AEEPORT_WAIT == ret) {
        ISockPort_WriteableEx(pMe->pISockPort, &pMe->cbSendTo,
                              PosDetApp_TryWriteToSvr, pMe);
        return;
    }

    // an error occurred. get the error code.
    if (AEEPORT_ERROR == ret) {
        ret = ISockPort_GetLastError(pMe->pISockPort);
        DBGPRINTF("SockPort write err = %d", ret);
        // server closed the connection, close the applet.
        if (AEE_NET_ECONNRESET == ret) {
            DBGPRINTF("Server closed the connection!");
            ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        }
        return;
    }

    // connection closed by the other side
    if (AEEPORT_CLOSED == ret) {
        DBGPRINTF("Server closed the connection!");
        ISHELL_CloseApplet(pMe->applet.m_pIShell, FALSE);
        return;
    }

    // some bytes were written.
    pMe->uBytesSent += ret;

    // Not all the bytes were written yet. Call PosDetApp_TryWriteToSvr() again
    // when the write operation may progress.
    if (pMe->uBytesSent < SOCK_BUF_SIZE) {
        ISockPort_WriteableEx(pMe->pISockPort, &pMe->cbSendTo,
                              PosDetApp_TryWriteToSvr, pMe);
        return;
    }

    // (SOCK_BUF_SIZE == pMe->uBytesSent) - all the bytes were successfully
    // written reset the bytes counter for next write operation
    pMe->uBytesSent = 0;
}

static void
PosDetApp_ProcessGPSData(PosDetApp *pMe)
{
    int err = 0;
    err = PosDetApp_DecodePosInfo(pMe);
    if (err != SUCCESS) {
        DBGPRINTF("Decode posInfo failed: err = %d", err);
        return;
    }
    PosDetApp_MakeReportStr(pMe);

    PosDetApp_TryWriteToSvr(pMe);

    /* test */
    PosDetApp_ShowGPSInfo(pMe);
    PosDetApp_LogPos(pMe);
}

static boolean
PosDetApp_RequestAFix(PosDetApp *pMe)
{
    /* Request a fix. */
    /* TODO: We can choose MultipleRequests or SingleRequest according to the
       settings in the configuration file. */
    if (pMe->gpsSettings.reqType == MULTIPLE_REQUESTS) {
        PosDetApp_CnfgTrackNetwork(pMe);
        CALLBACK_Init(&pMe->cbReqInterval, PosDetApp_MultipleRequests, pMe);
        return PosDetApp_MultipleRequests(pMe);
    }
    else if (pMe->gpsSettings.reqType == SINGLE_REQUEST) {
        return PosDetApp_SingleRequest(pMe);
    }
    else {
        return TRUE;
    }
}

/* Return:
     SUCCESS
     NO_USER_CONFIG
     ENOMEMORY
     other file manipulation error from IFILEMGR_GetLastError().
*/
static int
PosDetApp_ReadUserConfig(PosDetApp *pMe)
{
    IFile *pCnfgFile = NULL;
    int ret = SUCCESS;
    char *pBuf = NULL;
    char *pszTok = NULL;
    char *pszDelimiter = ";";

    AEEFileInfo fileInfo;
    int nRead = 0;

    if (IFILEMGR_Test(pMe->pIFileMgr, SPD_CONFIG_FILE) != SUCCESS) {
        return NO_USER_CONFIG;
    }
    else {
        pCnfgFile = IFILEMGR_OpenFile(pMe->pIFileMgr, SPD_CONFIG_FILE,
                                      _OFM_READ);
        if (NULL == pCnfgFile) {
            ret = IFILEMGR_GetLastError(pMe->pIFileMgr);
            return ret;
        }
    }

    (void)IFILE_GetInfo(pCnfgFile, &fileInfo);
    pBuf = (char*)MALLOC(fileInfo.dwSize);
    if (NULL == pBuf) {
        return ENOMEMORY;
    }

    nRead = IFILE_Read(pCnfgFile, (void*)pBuf, fileInfo.dwSize);
    if ((uint32)nRead != fileInfo.dwSize) {
        FREEIF(pBuf);
        ret = IFILEMGR_GetLastError(pMe->pIFileMgr);
        return ret;
    }

    /* Check for upload server IP. */
    pszTok = STRSTR(pBuf, SPD_CONFIG_UPLOAD_SVR_IP);
    if (pszTok) {
        char *pszSvr = NULL;
        int nResult = 0;
        pszTok += STRLEN(SPD_CONFIG_UPLOAD_SVR_IP);
        nResult = DistToSemi(pszTok);
        pszSvr = (char*)MALLOC(nResult + 1);
        (void)STRLCPY(pszSvr, pszTok, nResult + 1);
        if (!INET_PTON(AEE_AF_INET, pszSvr, &pMe->uploadSvr.addr)) {
            FREE(pszSvr);
            FREEIF(pBuf);
            return EFAILED;
        }
        FREE(pszSvr);
    }

    /* Check for upload server port. */
    pszTok = STRSTR(pBuf, SPD_CONFIG_UPLOAD_SVR_PORT);
    if (pszTok) {
        INPort temp;
        pszTok += STRLEN(SPD_CONFIG_UPLOAD_SVR_PORT);
        temp = (INPort)STRTOUL(pszTok, &pszDelimiter, 10);
        pMe->uploadSvr.port = AEE_htons(temp);
    }

    /* Check for max times of connection try. */
    pszTok = STRSTR(pBuf, SPD_CONFIG_CONNECT_MAX_TRY);
    if (pszTok) {
        pszTok += STRLEN(SPD_CONFIG_CONNECT_MAX_TRY);
        pMe->tcpConnMaxTry = (int)STRTOUL(pszTok, &pszDelimiter, 10);
    }

    IFILE_Release(pCnfgFile);
    return ret;
}

static void
PosDetApp_ApplyDefaultConfig(PosDetApp *pMe)
{
    /* Initialize the addresses. */
    pMe->sockAddr.wFamily = AEE_AF_INET;          /* IPv4 socket */
    pMe->sockAddr.inet.port = HTONS(SERVER_PORT); /* set port number */
    INET_PTON(pMe->sockAddr.wFamily, SERVER_ADDR,
              &(pMe->sockAddr.inet.addr)); /* set server IP addr */

    /* Initialize the max times of connect try. */
    pMe->tcpConnMaxTry = CONNECT_MAX_TRY;
}
