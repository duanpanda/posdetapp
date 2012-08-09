#include "RyanUtils.h"

/*===========================================================================
HELPER ROUTINES FOR DRAWING.
===========================================================================*/

void
xDrawTextA(IDisplay *pd,AEEFont fnt, const char *pszText, int nChars,
           int x, int y, const AEERect *prcBackground, uint32 dwFlags)
{
    AECHAR wcBuf[80];

    if (nChars < 0)
        nChars = STRLEN(pszText);
    nChars = MIN(nChars, ARRAYSIZE(wcBuf));

    STR_TO_WSTR((char*)pszText, wcBuf, sizeof(wcBuf));

    (void)IDISPLAY_DrawText(pd, fnt, wcBuf, nChars, x, y, prcBackground, dwFlags);
}

void
xDisplay(AEEApplet *pMe, int nLine, int nCol, AEEFont fnt, uint32 dwFlags,
         const char *psz)
{
    AEEDeviceInfo di;
    AEERect rc;
    int nMaxLines;

    ISHELL_GetDeviceInfo(pMe->m_pIShell, &di);
    nMaxLines = (di.cyScreen / LINEHEIGHT) - 2;
    if (nMaxLines < 1)
        nMaxLines = 1;

    rc.x = nCol;
    rc.dx = di.cxScreen - nCol;

    rc.y = nLine * LINEHEIGHT;
    if( dwFlags & IDF_ALIGNVERT_MASK ) {
        rc.dy = di.cyScreen - rc.y;
    }
    else {
        rc.dy = LINEHEIGHT;
    }

    xDrawTextA(pMe->m_pIDisplay, fnt, psz, -1, rc.x, rc.y, &rc, dwFlags);

    IDISPLAY_Update(pMe->m_pIDisplay);
}

/*=======================================================================
Function: DistToSemi()

Description:
   Utility function that determines index of the first semicolon in the
   input string.

Prototype:

   int DistToSemi(const char * pszStr);

Parameters:
   pszStr: [in]. String to check.

Return Value:

   The index of the first semicolon.

Comments:
   None

Side Effects:
   None

See Also:
   None
=======================================================================*/
int
DistToSemi(const char *pszStr)
{
    int nCount = 0;

    if (!pszStr) {
        return -1;
    }

    while (*pszStr != 0) {
        if (*pszStr == ';') {
            return nCount;
        }
        else {
            nCount++;
            pszStr++;
        }
    }

    return -1;
}
