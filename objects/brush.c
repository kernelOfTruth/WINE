/*
 * GDI brush objects
 *
 * Copyright 1993, 1994  Alexandre Julliard
 */

#include <stdlib.h>
#include "brush.h"
#include "bitmap.h"
#include "syscolor.h"
#include "metafile.h"
#include "color.h"
#include "stddebug.h"
#include "debug.h"


/***********************************************************************
 *           CreateBrushIndirect16    (GDI.50)
 */
HBRUSH16 WINAPI CreateBrushIndirect16( const LOGBRUSH16 * brush )
{
    BRUSHOBJ * brushPtr;
    HBRUSH16 hbrush = GDI_AllocObject( sizeof(BRUSHOBJ), BRUSH_MAGIC );
    if (!hbrush) return 0;
    brushPtr = (BRUSHOBJ *) GDI_HEAP_LOCK( hbrush );
    brushPtr->logbrush.lbStyle = brush->lbStyle;
    brushPtr->logbrush.lbColor = brush->lbColor;
    brushPtr->logbrush.lbHatch = brush->lbHatch;
    GDI_HEAP_UNLOCK( hbrush );
    return hbrush;
}


/***********************************************************************
 *           CreateBrushIndirect32    (GDI32.27)
 */
HBRUSH32 WINAPI CreateBrushIndirect32( const LOGBRUSH32 * brush )
{
    BRUSHOBJ * brushPtr;
    HBRUSH32 hbrush = GDI_AllocObject( sizeof(BRUSHOBJ), BRUSH_MAGIC );
    if (!hbrush) return 0;
    brushPtr = (BRUSHOBJ *) GDI_HEAP_LOCK( hbrush );
    brushPtr->logbrush.lbStyle = brush->lbStyle;
    brushPtr->logbrush.lbColor = brush->lbColor;
    brushPtr->logbrush.lbHatch = brush->lbHatch;
    GDI_HEAP_UNLOCK( hbrush );
    return hbrush;
}


/***********************************************************************
 *           CreateHatchBrush16    (GDI.58)
 */
HBRUSH16 WINAPI CreateHatchBrush16( INT16 style, COLORREF color )
{
    LOGBRUSH32 logbrush = { BS_HATCHED, color, style };
    dprintf_gdi(stddeb, "CreateHatchBrush16: %d %06lx\n", style, color );
    if ((style < 0) || (style >= NB_HATCH_STYLES)) return 0;
    return CreateBrushIndirect32( &logbrush );
}


/***********************************************************************
 *           CreateHatchBrush32    (GDI32.48)
 */
HBRUSH32 WINAPI CreateHatchBrush32( INT32 style, COLORREF color )
{
    LOGBRUSH32 logbrush = { BS_HATCHED, color, style };
    dprintf_gdi(stddeb, "CreateHatchBrush32: %d %06lx\n", style, color );
    if ((style < 0) || (style >= NB_HATCH_STYLES)) return 0;
    return CreateBrushIndirect32( &logbrush );
}


/***********************************************************************
 *           CreatePatternBrush16    (GDI.60)
 */
HBRUSH16 WINAPI CreatePatternBrush16( HBITMAP16 hbitmap )
{
    return (HBRUSH16)CreatePatternBrush32( hbitmap );
}


/***********************************************************************
 *           CreatePatternBrush32    (GDI32.54)
 */
HBRUSH32 WINAPI CreatePatternBrush32( HBITMAP32 hbitmap )
{
    LOGBRUSH32 logbrush = { BS_PATTERN, 0, 0 };
    BITMAPOBJ *bmp, *newbmp;

    dprintf_gdi(stddeb, "CreatePatternBrush: %04x\n", hbitmap );

      /* Make a copy of the bitmap */

    if (!(bmp = (BITMAPOBJ *) GDI_GetObjPtr( hbitmap, BITMAP_MAGIC )))
	return 0;
    logbrush.lbHatch = (INT32)CreateBitmapIndirect16( &bmp->bitmap );
    newbmp = (BITMAPOBJ *) GDI_GetObjPtr( (HGDIOBJ32)logbrush.lbHatch,
                                          BITMAP_MAGIC );
    if (!newbmp) 
    {
      GDI_HEAP_UNLOCK( hbitmap );
      return 0;
    }
    XCopyArea( display, bmp->pixmap, newbmp->pixmap, BITMAP_GC(bmp),
	       0, 0, bmp->bitmap.bmWidth, bmp->bitmap.bmHeight, 0, 0 );
    GDI_HEAP_UNLOCK( hbitmap );
    GDI_HEAP_UNLOCK( logbrush.lbHatch );
    return CreateBrushIndirect32( &logbrush );
}


/***********************************************************************
 *           CreateDIBPatternBrush16    (GDI.445)
 */
HBRUSH16 WINAPI CreateDIBPatternBrush16( HGLOBAL16 hbitmap, UINT16 coloruse )
{
    LOGBRUSH32 logbrush = { BS_DIBPATTERN, coloruse, 0 };
    BITMAPINFO *info, *newInfo;
    INT32 size;
    
    dprintf_gdi(stddeb, "CreateDIBPatternBrush: %04x\n", hbitmap );

      /* Make a copy of the bitmap */

    if (!(info = (BITMAPINFO *)GlobalLock16( hbitmap ))) return 0;

    if (info->bmiHeader.biCompression)
        size = info->bmiHeader.biSizeImage;
    else
	size = (info->bmiHeader.biWidth * info->bmiHeader.biBitCount + 31) / 32
	         * 8 * info->bmiHeader.biHeight;
    size += DIB_BitmapInfoSize( info, coloruse );

    if (!(logbrush.lbHatch = (INT16)GlobalAlloc16( GMEM_MOVEABLE, size )))
    {
	GlobalUnlock16( hbitmap );
	return 0;
    }
    newInfo = (BITMAPINFO *) GlobalLock16( (HGLOBAL16)logbrush.lbHatch );
    memcpy( newInfo, info, size );
    GlobalUnlock16( (HGLOBAL16)logbrush.lbHatch );
    GlobalUnlock16( hbitmap );
    return CreateBrushIndirect32( &logbrush );
}


/***********************************************************************
 *           CreateDIBPatternBrush32    (GDI32.34)
 */
HBRUSH32 WINAPI CreateDIBPatternBrush32( HGLOBAL32 hbitmap, UINT32 coloruse )
{
    LOGBRUSH32 logbrush = { BS_DIBPATTERN, coloruse, 0 };
    BITMAPINFO *info, *newInfo;
    INT32 size;
    
    dprintf_gdi(stddeb, "CreateDIBPatternBrush: %04x\n", hbitmap );

      /* Make a copy of the bitmap */

    if (!(info = (BITMAPINFO *)GlobalLock32( hbitmap ))) return 0;

    if (info->bmiHeader.biCompression)
        size = info->bmiHeader.biSizeImage;
    else
	size = (info->bmiHeader.biWidth * info->bmiHeader.biBitCount + 31) / 32
	         * 8 * info->bmiHeader.biHeight;
    size += DIB_BitmapInfoSize( info, coloruse );

    if (!(logbrush.lbHatch = (INT32)GlobalAlloc16( GMEM_MOVEABLE, size )))
    {
	GlobalUnlock16( hbitmap );
	return 0;
    }
    newInfo = (BITMAPINFO *) GlobalLock16( (HGLOBAL16)logbrush.lbHatch );
    memcpy( newInfo, info, size );
    GlobalUnlock16( (HGLOBAL16)logbrush.lbHatch );
    GlobalUnlock16( hbitmap );
    return CreateBrushIndirect32( &logbrush );
}


/***********************************************************************
 *           CreateSolidBrush    (GDI.66)
 */
HBRUSH16 WINAPI CreateSolidBrush16( COLORREF color )
{
    LOGBRUSH32 logbrush = { BS_SOLID, color, 0 };
    dprintf_gdi(stddeb, "CreateSolidBrush16: %06lx\n", color );
    return CreateBrushIndirect32( &logbrush );
}


/***********************************************************************
 *           CreateSolidBrush32    (GDI32.64)
 */
HBRUSH32 WINAPI CreateSolidBrush32( COLORREF color )
{
    LOGBRUSH32 logbrush = { BS_SOLID, color, 0 };
    dprintf_gdi(stddeb, "CreateSolidBrush32: %06lx\n", color );
    return CreateBrushIndirect32( &logbrush );
}


/***********************************************************************
 *           SetBrushOrg    (GDI.148)
 */
DWORD WINAPI SetBrushOrg( HDC16 hdc, INT16 x, INT16 y )
{
    DWORD retval;
    DC * dc = (DC *) GDI_GetObjPtr( hdc, DC_MAGIC );
    if (!dc) return FALSE;
    retval = dc->w.brushOrgX | (dc->w.brushOrgY << 16);
    dc->w.brushOrgX = x;
    dc->w.brushOrgY = y;
    return retval;
}


/***********************************************************************
 *           SetBrushOrgEx    (GDI32.308)
 */
BOOL32 WINAPI SetBrushOrgEx( HDC32 hdc, INT32 x, INT32 y, LPPOINT32 oldorg )
{
    DC * dc = (DC *) GDI_GetObjPtr( hdc, DC_MAGIC );

    if (!dc) return FALSE;
    if (oldorg)
    {
        oldorg->x = dc->w.brushOrgX;
        oldorg->y = dc->w.brushOrgY;
    }
    dc->w.brushOrgX = x;
    dc->w.brushOrgY = y;
    return TRUE;
}

/***********************************************************************
 *           FixBrushOrgEx    (GDI32.102)
 * SDK says discontinued, but in Win95 GDI32 this is the same as SetBrushOrgEx
 */
BOOL32 WINAPI FixBrushOrgEx( HDC32 hdc, INT32 x, INT32 y, LPPOINT32 oldorg )
{
    return SetBrushOrgEx(hdc,x,y,oldorg);
}


/***********************************************************************
 *           GetSysColorBrush16    (USER.281)
 */
HBRUSH16 WINAPI GetSysColorBrush16( INT16 index )
{
    return (HBRUSH16)GetSysColorBrush32(index);
}


/***********************************************************************
 *           GetSysColorBrush32    (USER32.289)
 */
HBRUSH32 WINAPI GetSysColorBrush32( INT32 index )
{
  switch(index){
  case COLOR_SCROLLBAR:
    return sysColorObjects.hbrushScrollbar;
  case COLOR_BACKGROUND: 
    return sysColorObjects.hbrushBackground; 
  case COLOR_ACTIVECAPTION:
    return sysColorObjects.hbrushActiveCaption;
  case COLOR_INACTIVECAPTION:
    return sysColorObjects.hbrushInactiveCaption;
  case COLOR_MENU:
    return sysColorObjects.hbrushMenu;
  case COLOR_WINDOW:
    return sysColorObjects.hbrushWindow;
  case COLOR_WINDOWFRAME:
    return sysColorObjects.hbrushWindowFrame;
  case COLOR_MENUTEXT:
    return sysColorObjects.hbrushMenuText;
  case COLOR_WINDOWTEXT:
    return sysColorObjects.hbrushWindowText;
  case COLOR_CAPTIONTEXT:
    return sysColorObjects.hbrushCaptionText;
  case COLOR_ACTIVEBORDER:
    return sysColorObjects.hbrushActiveBorder;
  case COLOR_INACTIVEBORDER:
    return sysColorObjects.hbrushInactiveBorder;
  case COLOR_APPWORKSPACE:
    return sysColorObjects.hbrushAppWorkspace; 
  case COLOR_HIGHLIGHT:
    return sysColorObjects.hbrushHighlight;
  case COLOR_HIGHLIGHTTEXT:
    return sysColorObjects.hbrushHighlightText;
  case COLOR_BTNFACE: /* same as COLOR_3DFACE */
    return sysColorObjects.hbrushBtnFace;
  case COLOR_BTNSHADOW: /* same as COLOR_3DSHADOW */
    return sysColorObjects.hbrushBtnShadow;
  case COLOR_GRAYTEXT:
    return sysColorObjects.hbrushGrayText;
  case COLOR_BTNTEXT:
    return sysColorObjects.hbrushBtnText;
  case COLOR_INACTIVECAPTIONTEXT:
    return sysColorObjects.hbrushInactiveCaptionText;
  case COLOR_BTNHIGHLIGHT: /* same as COLOR_(3DHIGH|3DHI|BTNHI)LIGHT */
    return sysColorObjects.hbrushBtnHighlight;
  case COLOR_3DDKSHADOW:
    return sysColorObjects.hbrush3DDkShadow;
  case COLOR_3DLIGHT:
    return sysColorObjects.hbrush3DLight;
  case COLOR_INFOTEXT:
    return sysColorObjects.hbrushInfoText;
  case COLOR_INFOBK:
    return sysColorObjects.hbrushInfoBk;
  default:
    fprintf( stderr, "GetSysColorBrush32: Unknown index(%d)\n", index );
  }

  return GetStockObject32(LTGRAY_BRUSH);

}


/***********************************************************************
 *           BRUSH_DeleteObject
 */
BOOL32 BRUSH_DeleteObject( HBRUSH16 hbrush, BRUSHOBJ * brush )
{
    switch(brush->logbrush.lbStyle)
    {
      case BS_PATTERN:
	  DeleteObject32( (HGDIOBJ32)brush->logbrush.lbHatch );
	  break;
      case BS_DIBPATTERN:
	  GlobalFree16( (HGLOBAL16)brush->logbrush.lbHatch );
	  break;
    }
    return GDI_FreeObject( hbrush );
}


/***********************************************************************
 *           BRUSH_GetObject16
 */
INT16 BRUSH_GetObject16( BRUSHOBJ * brush, INT16 count, LPSTR buffer )
{
    LOGBRUSH16 logbrush;

    logbrush.lbStyle = brush->logbrush.lbStyle;
    logbrush.lbColor = brush->logbrush.lbColor;
    logbrush.lbHatch = brush->logbrush.lbHatch;
    if (count > sizeof(logbrush)) count = sizeof(logbrush);
    memcpy( buffer, &logbrush, count );
    return count;
}


/***********************************************************************
 *           BRUSH_GetObject32
 */
INT32 BRUSH_GetObject32( BRUSHOBJ * brush, INT32 count, LPSTR buffer )
{
    if (count > sizeof(brush->logbrush)) count = sizeof(brush->logbrush);
    memcpy( buffer, &brush->logbrush, count );
    return count;
}
