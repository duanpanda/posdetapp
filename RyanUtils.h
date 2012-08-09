#include "AEEStdLib.h"
#include "AEEDisp.h"
#include "AEEAppGen.h"

#define LINEHEIGHT 16
#define TOPLINE    0
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))

void xDrawTextA(IDisplay * pd, AEEFont fnt, const char *pszText, int nChars,
                int x,int y, const AEERect *prcBackground, uint32 dwFlags);

void xDisplay(AEEApplet *pMe, int nLine, int nCol, AEEFont fnt, uint32 dwFlags,
              const char *psz);

int DistToSemi(const char *pszStr);
