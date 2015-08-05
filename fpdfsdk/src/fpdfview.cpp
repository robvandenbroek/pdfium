// Copyright 2014 PDFium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "../../core/include/fpdfapi/fpdf_module.h"
#include "../../core/include/fxcodec/fx_codec.h"
#include "../../core/include/fxcrt/fx_safe_types.h"
#include "../../public/fpdf_ext.h"
#include "../../public/fpdf_formfill.h"
#include "../../public/fpdf_progressive.h"
#include "../../public/fpdfview.h"
#include "../../third_party/base/nonstd_unique_ptr.h"
#include "../../third_party/base/numerics/safe_conversions_impl.h"
#include "../include/fsdk_define.h"
#include "../include/fsdk_mgr.h"
#include "../include/fsdk_rendercontext.h"
#include "../include/fpdfxfa/fpdfxfa_doc.h"
#include "../include/fpdfxfa/fpdfxfa_app.h"
#include "../include/fpdfxfa/fpdfxfa_page.h"
#include "../include/fpdfxfa/fpdfxfa_util.h"

CFPDF_FileStream::CFPDF_FileStream(FPDF_FILEHANDLER* pFS) {
  m_pFS = pFS;
  m_nCurPos = 0;
}

IFX_FileStream* CFPDF_FileStream::Retain() {
  return this;
}

void CFPDF_FileStream::Release() {
  if (m_pFS && m_pFS->Release)
    m_pFS->Release(m_pFS->clientData);
  delete this;
}

FX_FILESIZE CFPDF_FileStream::GetSize() {
  if (m_pFS && m_pFS->GetSize)
    return (FX_FILESIZE)m_pFS->GetSize(m_pFS->clientData);
  return 0;
}

FX_BOOL CFPDF_FileStream::IsEOF() {
  return m_nCurPos >= GetSize();
}

FX_BOOL CFPDF_FileStream::ReadBlock(void* buffer,
                                    FX_FILESIZE offset,
                                    size_t size) {
  if (!buffer || !size || !m_pFS->ReadBlock)
    return FALSE;

  if (m_pFS->ReadBlock(m_pFS->clientData, (FPDF_DWORD)offset, buffer,
                       (FPDF_DWORD)size) == 0) {
    m_nCurPos = offset + size;
    return TRUE;
  }
  return FALSE;
}

size_t CFPDF_FileStream::ReadBlock(void* buffer, size_t size) {
  if (!buffer || !size || !m_pFS->ReadBlock)
    return 0;

  FX_FILESIZE nSize = GetSize();
  if (m_nCurPos >= nSize)
    return 0;
  FX_FILESIZE dwAvail = nSize - m_nCurPos;
  if (dwAvail < (FX_FILESIZE)size)
    size = (size_t)dwAvail;
  if (m_pFS->ReadBlock(m_pFS->clientData, (FPDF_DWORD)m_nCurPos, buffer,
                       (FPDF_DWORD)size) == 0) {
    m_nCurPos += size;
    return size;
  }

  return 0;
}

FX_BOOL CFPDF_FileStream::WriteBlock(const void* buffer,
                                     FX_FILESIZE offset,
                                     size_t size) {
  if (!m_pFS || !m_pFS->WriteBlock)
    return FALSE;

  if (m_pFS->WriteBlock(m_pFS->clientData, (FPDF_DWORD)offset, buffer,
                        (FPDF_DWORD)size) == 0) {
    m_nCurPos = offset + size;
    return TRUE;
  }
  return FALSE;
}

FX_BOOL CFPDF_FileStream::Flush() {
  if (!m_pFS || !m_pFS->Flush)
    return TRUE;

  return m_pFS->Flush(m_pFS->clientData) == 0;
}

CPDF_CustomAccess::CPDF_CustomAccess(FPDF_FILEACCESS* pFileAccess) {
  m_FileAccess = *pFileAccess;
  m_BufferOffset = (FX_DWORD)-1;
}

FX_BOOL CPDF_CustomAccess::GetByte(FX_DWORD pos, uint8_t& ch) {
  if (pos >= m_FileAccess.m_FileLen)
    return FALSE;
  if (m_BufferOffset == (FX_DWORD)-1 || pos < m_BufferOffset ||
      pos >= m_BufferOffset + 512) {
    // Need to read from file access
    m_BufferOffset = pos;
    int size = 512;
    if (pos + 512 > m_FileAccess.m_FileLen)
      size = m_FileAccess.m_FileLen - pos;
    if (!m_FileAccess.m_GetBlock(m_FileAccess.m_Param, m_BufferOffset, m_Buffer,
                                 size))
      return FALSE;
  }
  ch = m_Buffer[pos - m_BufferOffset];
  return TRUE;
}

FX_BOOL CPDF_CustomAccess::GetBlock(FX_DWORD pos,
                                    uint8_t* pBuf,
                                    FX_DWORD size) {
  if (pos + size > m_FileAccess.m_FileLen)
    return FALSE;
  return m_FileAccess.m_GetBlock(m_FileAccess.m_Param, pos, pBuf, size);
}

FX_BOOL CPDF_CustomAccess::ReadBlock(void* buffer,
                                     FX_FILESIZE offset,
                                     size_t size) {
  if (offset < 0) {
    return FALSE;
  }
  FX_SAFE_FILESIZE newPos =
      pdfium::base::checked_cast<FX_FILESIZE, size_t>(size);
  newPos += offset;
  if (!newPos.IsValid() || newPos.ValueOrDie() > m_FileAccess.m_FileLen) {
    return FALSE;
  }
  return m_FileAccess.m_GetBlock(m_FileAccess.m_Param, offset, (uint8_t*)buffer,
                                 size);
}

// 0 bit: FPDF_POLICY_MACHINETIME_ACCESS
static FX_DWORD foxit_sandbox_policy = 0xFFFFFFFF;

void FSDK_SetSandBoxPolicy(FPDF_DWORD policy, FPDF_BOOL enable) {
  switch (policy) {
    case FPDF_POLICY_MACHINETIME_ACCESS: {
      if (enable)
        foxit_sandbox_policy |= 0x01;
      else
        foxit_sandbox_policy &= 0xFFFFFFFE;
    } break;
    default:
      break;
  }
}

FPDF_BOOL FSDK_IsSandBoxPolicyEnabled(FPDF_DWORD policy) {
  switch (policy) {
    case FPDF_POLICY_MACHINETIME_ACCESS: {
      if (foxit_sandbox_policy & 0x01)
        return TRUE;
      else
        return FALSE;
    } break;
    default:
      break;
  }
  return FALSE;
}

CCodec_ModuleMgr* g_pCodecModule = nullptr;

DLLEXPORT void STDCALL FPDF_InitLibrary() {
  g_pCodecModule = new CCodec_ModuleMgr();

  CFX_GEModule::Create();
  CFX_GEModule::Get()->SetCodecModule(g_pCodecModule);

  CPDF_ModuleMgr::Create();
  CPDF_ModuleMgr::Get()->SetCodecModule(g_pCodecModule);
  CPDF_ModuleMgr::Get()->InitPageModule();
  CPDF_ModuleMgr::Get()->InitRenderModule();

  CPDFXFA_App::GetInstance()->Initialize();
}

DLLEXPORT void STDCALL FPDF_DestroyLibrary() {
  CPDFXFA_App::ReleaseInstance();
  CPDF_ModuleMgr::Destroy();
  CFX_GEModule::Destroy();

  delete g_pCodecModule;
  g_pCodecModule = nullptr;
}

#ifndef _WIN32
int g_LastError;
void SetLastError(int err) {
  g_LastError = err;
}

int GetLastError() {
  return g_LastError;
}
#endif

void ProcessParseError(FX_DWORD err_code) {
  // Translate FPDFAPI error code to FPDFVIEW error code
  switch (err_code) {
    case PDFPARSE_ERROR_FILE:
      err_code = FPDF_ERR_FILE;
      break;
    case PDFPARSE_ERROR_FORMAT:
      err_code = FPDF_ERR_FORMAT;
      break;
    case PDFPARSE_ERROR_PASSWORD:
      err_code = FPDF_ERR_PASSWORD;
      break;
    case PDFPARSE_ERROR_HANDLER:
      err_code = FPDF_ERR_SECURITY;
      break;
  }
  SetLastError(err_code);
}

DLLEXPORT void STDCALL FPDF_SetSandBoxPolicy(FPDF_DWORD policy,
                                             FPDF_BOOL enable) {
  return FSDK_SetSandBoxPolicy(policy, enable);
}


DLLEXPORT FPDF_DOCUMENT STDCALL FPDF_LoadDocument(FPDF_STRING file_path,
                                                  FPDF_BYTESTRING password) {
  // NOTE: the creation of the file needs to be by the embedder on the
  // other side of this API.
  IFX_FileRead* pFileAccess = FX_CreateFileRead((const FX_CHAR*)file_path);
  if (!pFileAccess) {
    return nullptr;
  }

  CPDF_Parser* pParser = new CPDF_Parser;
  pParser->SetPassword(password);

  FX_DWORD err_code = pParser->StartParse(pFileAccess);
  if (err_code) {
    delete pParser;
    ProcessParseError(err_code);
    return NULL;
  }
  CPDF_Document* pPDFDoc = pParser->GetDocument();
  if (!pPDFDoc)
    return NULL;

  CPDFXFA_App* pProvider = CPDFXFA_App::GetInstance();
  CPDFXFA_Document* pDocument = new CPDFXFA_Document(pPDFDoc, pProvider);
  return pDocument;
}

DLLEXPORT FPDF_BOOL STDCALL FPDF_HasXFAField(FPDF_DOCUMENT document,
                                             int* docType) {
  if (!document)
    return FALSE;

  CPDF_Document* pdfDoc =
      (static_cast<CPDFXFA_Document*>(document))->GetPDFDoc();
  if (!pdfDoc)
    return FALSE;

  CPDF_Dictionary* pRoot = pdfDoc->GetRoot();
  if (!pRoot)
    return FALSE;

  CPDF_Dictionary* pAcroForm = pRoot->GetDict("AcroForm");
  if (!pAcroForm)
    return FALSE;

  CPDF_Object* pXFA = pAcroForm->GetElement("XFA");
  if (!pXFA)
    return FALSE;

  FX_BOOL bDynamicXFA = pRoot->GetBoolean("NeedsRendering", FALSE);

  if (bDynamicXFA)
    *docType = DOCTYPE_DYNIMIC_XFA;
  else
    *docType = DOCTYPE_STATIC_XFA;

  return TRUE;
}

DLLEXPORT FPDF_BOOL STDCALL FPDF_LoadXFA(FPDF_DOCUMENT document) {
  return document && (static_cast<CPDFXFA_Document*>(document))->LoadXFADoc();
}

extern void CheckUnSupportError(CPDF_Document* pDoc, FX_DWORD err_code);

class CMemFile final : public IFX_FileRead {
 public:
  CMemFile(uint8_t* pBuf, FX_FILESIZE size) : m_pBuf(pBuf), m_size(size) {}

  virtual void Release() { delete this; }
  virtual FX_FILESIZE GetSize() { return m_size; }
  virtual FX_BOOL ReadBlock(void* buffer, FX_FILESIZE offset, size_t size) {
    if (offset < 0) {
      return FALSE;
    }
    FX_SAFE_FILESIZE newPos =
        pdfium::base::checked_cast<FX_FILESIZE, size_t>(size);
    newPos += offset;
    if (!newPos.IsValid() || newPos.ValueOrDie() > (FX_DWORD)m_size) {
      return FALSE;
    }
    FXSYS_memcpy(buffer, m_pBuf + offset, size);
    return TRUE;
  }

 private:
  uint8_t* m_pBuf;
  FX_FILESIZE m_size;
};
DLLEXPORT FPDF_DOCUMENT STDCALL FPDF_LoadMemDocument(const void* data_buf,
                                                     int size,
                                                     FPDF_BYTESTRING password) {
  CPDF_Parser* pParser = new CPDF_Parser;
  pParser->SetPassword(password);
  CMemFile* pMemFile = new CMemFile((uint8_t*)data_buf, size);
  FX_DWORD err_code = pParser->StartParse(pMemFile);
  if (err_code) {
    delete pParser;
    ProcessParseError(err_code);
    return NULL;
  }
  CPDF_Document* pDoc = NULL;
  pDoc = pParser ? pParser->GetDocument() : NULL;
  CheckUnSupportError(pDoc, err_code);
  CPDF_Document* pPDFDoc = pParser->GetDocument();
  if (!pPDFDoc)
    return NULL;

  CPDFXFA_App* pProvider = CPDFXFA_App::GetInstance();
  CPDFXFA_Document* pDocument = new CPDFXFA_Document(pPDFDoc, pProvider);
  return pDocument;
}

DLLEXPORT FPDF_DOCUMENT STDCALL
FPDF_LoadCustomDocument(FPDF_FILEACCESS* pFileAccess,
                        FPDF_BYTESTRING password) {
  CPDF_Parser* pParser = new CPDF_Parser;
  pParser->SetPassword(password);
  CPDF_CustomAccess* pFile = new CPDF_CustomAccess(pFileAccess);
  FX_DWORD err_code = pParser->StartParse(pFile);
  if (err_code) {
    delete pParser;
    ProcessParseError(err_code);
    return NULL;
  }
  CPDF_Document* pDoc = NULL;
  pDoc = pParser ? pParser->GetDocument() : NULL;
  CheckUnSupportError(pDoc, err_code);
  CPDF_Document* pPDFDoc = pParser->GetDocument();
  if (!pPDFDoc)
    return NULL;

  CPDFXFA_App* pProvider = CPDFXFA_App::GetInstance();
  CPDFXFA_Document* pDocument = new CPDFXFA_Document(pPDFDoc, pProvider);
  return pDocument;
}

DLLEXPORT FPDF_BOOL STDCALL FPDF_GetFileVersion(FPDF_DOCUMENT doc,
                                                int* fileVersion) {
  if (!doc || !fileVersion)
    return FALSE;
  *fileVersion = 0;
  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)doc;
  CPDF_Document* pPDFDoc = pDoc->GetPDFDoc();
  if (!pPDFDoc)
    return (FX_DWORD)-1;
  CPDF_Parser* pParser = (CPDF_Parser*)pPDFDoc->GetParser();

  if (!pParser)
    return FALSE;
  *fileVersion = pParser->GetFileVersion();
  return TRUE;
}

// jabdelmalek: changed return type from FX_DWORD to build on Linux (and match
// header).
DLLEXPORT unsigned long STDCALL FPDF_GetDocPermissions(FPDF_DOCUMENT document) {
  if (document == NULL)
    return 0;
  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)document;
  CPDF_Document* pPDFDoc = pDoc->GetPDFDoc();
  if (!pPDFDoc)
    return (FX_DWORD)-1;
  CPDF_Parser* pParser = (CPDF_Parser*)pPDFDoc->GetParser();
  CPDF_Dictionary* pDict = pParser->GetEncryptDict();
  if (pDict == NULL)
    return (FX_DWORD)-1;

  return pDict->GetInteger("P");
}

DLLEXPORT int STDCALL FPDF_GetSecurityHandlerRevision(FPDF_DOCUMENT document) {
  if (document == NULL)
    return -1;
  CPDF_Document* pDoc = ((CPDFXFA_Document*)document)->GetPDFDoc();
  CPDF_Parser* pParser = (CPDF_Parser*)pDoc->GetParser();
  CPDF_Dictionary* pDict = pParser->GetEncryptDict();
  if (pDict == NULL)
    return -1;

  return pDict->GetInteger("R");
}

DLLEXPORT int STDCALL FPDF_GetPageCount(FPDF_DOCUMENT document) {
  if (document == NULL)
    return 0;
  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)document;
  return pDoc->GetPageCount();
  //	return ((CPDF_Document*)document)->GetPageCount();
}

DLLEXPORT FPDF_PAGE STDCALL FPDF_LoadPage(FPDF_DOCUMENT document,
                                          int page_index) {
  if (document == NULL)
    return NULL;
  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)document;
  if (page_index < 0 || page_index >= pDoc->GetPageCount())
    return NULL;
  //	CPDF_Parser* pParser = (CPDF_Parser*)document;
  return pDoc->GetPage(page_index);
}

DLLEXPORT double STDCALL FPDF_GetPageWidth(FPDF_PAGE page) {
  if (!page)
    return 0.0;
  return ((CPDFXFA_Page*)page)->GetPageWidth();
  //	return ((CPDF_Page*)page)->GetPageWidth();
}

DLLEXPORT double STDCALL FPDF_GetPageHeight(FPDF_PAGE page) {
  if (!page)
    return 0.0;
  //	return ((CPDF_Page*)page)->GetPageHeight();
  return ((CPDFXFA_Page*)page)->GetPageHeight();
}

void DropContext(void* data) {
  delete (CRenderContext*)data;
}

#if defined(_DEBUG) || defined(DEBUG)
#define DEBUG_TRACE
#endif

#if defined(_WIN32)
DLLEXPORT void STDCALL FPDF_RenderPage(HDC dc,
                                       FPDF_PAGE page,
                                       int start_x,
                                       int start_y,
                                       int size_x,
                                       int size_y,
                                       int rotate,
                                       int flags) {
  if (page == NULL)
    return;
  CPDF_Page* pPage = ((CPDFXFA_Page*)page)->GetPDFPage();
  if (!pPage)
    return;

  CRenderContext* pContext = new CRenderContext;
  pPage->SetPrivateData((void*)1, pContext, DropContext);

#ifndef _WIN32_WCE
  CFX_DIBitmap* pBitmap = NULL;
  FX_BOOL bBackgroundAlphaNeeded = FALSE;
  bBackgroundAlphaNeeded = pPage->BackgroundAlphaNeeded();
  if (bBackgroundAlphaNeeded) {
    pBitmap = new CFX_DIBitmap;
    pBitmap->Create(size_x, size_y, FXDIB_Argb);
    pBitmap->Clear(0x00ffffff);
#ifdef _SKIA_SUPPORT_
    pContext->m_pDevice = new CFX_SkiaDevice;
    ((CFX_SkiaDevice*)pContext->m_pDevice)->Attach((CFX_DIBitmap*)pBitmap);
#else
    pContext->m_pDevice = new CFX_FxgeDevice;
    ((CFX_FxgeDevice*)pContext->m_pDevice)->Attach((CFX_DIBitmap*)pBitmap);
#endif
  } else
    pContext->m_pDevice = new CFX_WindowsDevice(dc);

  FPDF_RenderPage_Retail(pContext, page, start_x, start_y, size_x, size_y,
                         rotate, flags, TRUE, NULL);

  if (bBackgroundAlphaNeeded) {
    if (pBitmap) {
      CFX_WindowsDevice WinDC(dc);

      if (WinDC.GetDeviceCaps(FXDC_DEVICE_CLASS) == FXDC_PRINTER) {
        CFX_DIBitmap* pDst = new CFX_DIBitmap;
        int pitch = pBitmap->GetPitch();
        pDst->Create(size_x, size_y, FXDIB_Rgb32);
        FXSYS_memset(pDst->GetBuffer(), -1, pitch * size_y);
        pDst->CompositeBitmap(0, 0, size_x, size_y, pBitmap, 0, 0,
                              FXDIB_BLEND_NORMAL, NULL, FALSE, NULL);
        WinDC.StretchDIBits(pDst, 0, 0, size_x, size_y);
        delete pDst;
      } else
        WinDC.SetDIBits(pBitmap, 0, 0);
    }
  }
#else
  // get clip region
  RECT rect, cliprect;
  rect.left = start_x;
  rect.top = start_y;
  rect.right = start_x + size_x;
  rect.bottom = start_y + size_y;
  GetClipBox(dc, &cliprect);
  IntersectRect(&rect, &rect, &cliprect);
  int width = rect.right - rect.left;
  int height = rect.bottom - rect.top;

#ifdef DEBUG_TRACE
  {
    char str[128];
    memset(str, 0, sizeof(str));
    FXSYS_snprintf(str, sizeof(str) - 1, "Rendering DIB %d x %d", width,
                   height);
    CPDF_ModuleMgr::Get()->ReportError(999, str);
  }
#endif

  // Create a DIB section
  LPVOID pBuffer;
  BITMAPINFOHEADER bmih;
  FXSYS_memset(&bmih, 0, sizeof bmih);
  bmih.biSize = sizeof bmih;
  bmih.biBitCount = 24;
  bmih.biHeight = -height;
  bmih.biPlanes = 1;
  bmih.biWidth = width;
  pContext->m_hBitmap = CreateDIBSection(dc, (BITMAPINFO*)&bmih, DIB_RGB_COLORS,
                                         &pBuffer, NULL, 0);
  if (pContext->m_hBitmap == NULL) {
#if defined(DEBUG) || defined(_DEBUG)
    char str[128];
    memset(str, 0, sizeof(str));
    FXSYS_snprintf(str, sizeof(str) - 1,
                   "Error CreateDIBSection: %d x %d, error code = %d", width,
                   height, GetLastError());
    CPDF_ModuleMgr::Get()->ReportError(FPDFERR_OUT_OF_MEMORY, str);
#else
    CPDF_ModuleMgr::Get()->ReportError(FPDFERR_OUT_OF_MEMORY, NULL);
#endif
  }
  FXSYS_memset(pBuffer, 0xff, height * ((width * 3 + 3) / 4 * 4));

#ifdef DEBUG_TRACE
  { CPDF_ModuleMgr::Get()->ReportError(999, "DIBSection created"); }
#endif

  // Create a device with this external buffer
  pContext->m_pBitmap = new CFX_DIBitmap;
  pContext->m_pBitmap->Create(width, height, FXDIB_Rgb, (uint8_t*)pBuffer);
  pContext->m_pDevice = new CPDF_FxgeDevice;
  ((CPDF_FxgeDevice*)pContext->m_pDevice)->Attach(pContext->m_pBitmap);

#ifdef DEBUG_TRACE
  CPDF_ModuleMgr::Get()->ReportError(999, "Ready for PDF rendering");
#endif

  // output to bitmap device
  FPDF_RenderPage_Retail(pContext, page, start_x - rect.left,
                         start_y - rect.top, size_x, size_y, rotate, flags);

#ifdef DEBUG_TRACE
  CPDF_ModuleMgr::Get()->ReportError(999, "Finished PDF rendering");
#endif

  // Now output to real device
  HDC hMemDC = CreateCompatibleDC(dc);
  if (hMemDC == NULL) {
#if defined(DEBUG) || defined(_DEBUG)
    char str[128];
    memset(str, 0, sizeof(str));
    FXSYS_snprintf(str, sizeof(str) - 1,
                   "Error CreateCompatibleDC. Error code = %d", GetLastError());
    CPDF_ModuleMgr::Get()->ReportError(FPDFERR_OUT_OF_MEMORY, str);
#else
    CPDF_ModuleMgr::Get()->ReportError(FPDFERR_OUT_OF_MEMORY, NULL);
#endif
  }

  HGDIOBJ hOldBitmap = SelectObject(hMemDC, pContext->m_hBitmap);

#ifdef DEBUG_TRACE
  CPDF_ModuleMgr::Get()->ReportError(999, "Ready for screen rendering");
#endif

  BitBlt(dc, rect.left, rect.top, width, height, hMemDC, 0, 0, SRCCOPY);
  SelectObject(hMemDC, hOldBitmap);
  DeleteDC(hMemDC);

#ifdef DEBUG_TRACE
  CPDF_ModuleMgr::Get()->ReportError(999, "Finished screen rendering");
#endif

#endif
  if (bBackgroundAlphaNeeded) {
    delete pBitmap;
    pBitmap = NULL;
  }
  delete pContext;
  pPage->RemovePrivateData((void*)1);
}
#endif

DLLEXPORT void STDCALL FPDF_RenderPageBitmap(FPDF_BITMAP bitmap,
                                             FPDF_PAGE page,
                                             int start_x,
                                             int start_y,
                                             int size_x,
                                             int size_y,
                                             int rotate,
                                             int flags) {
  if (bitmap == NULL || page == NULL)
    return;
  CPDF_Page* pPage = ((CPDFXFA_Page*)page)->GetPDFPage();
  if (!pPage)
    return;

  CRenderContext* pContext = new CRenderContext;
  pPage->SetPrivateData((void*)1, pContext, DropContext);
#ifdef _SKIA_SUPPORT_
  pContext->m_pDevice = new CFX_SkiaDevice;

  if (flags & FPDF_REVERSE_BYTE_ORDER)
    ((CFX_SkiaDevice*)pContext->m_pDevice)
        ->Attach((CFX_DIBitmap*)bitmap, 0, TRUE);
  else
    ((CFX_SkiaDevice*)pContext->m_pDevice)->Attach((CFX_DIBitmap*)bitmap);
#else
  pContext->m_pDevice = new CFX_FxgeDevice;

  if (flags & FPDF_REVERSE_BYTE_ORDER)
    ((CFX_FxgeDevice*)pContext->m_pDevice)
        ->Attach((CFX_DIBitmap*)bitmap, 0, TRUE);
  else
    ((CFX_FxgeDevice*)pContext->m_pDevice)->Attach((CFX_DIBitmap*)bitmap);
#endif

  FPDF_RenderPage_Retail(pContext, page, start_x, start_y, size_x, size_y,
                         rotate, flags, TRUE, NULL);

  delete pContext;
  pPage->RemovePrivateData((void*)1);
}

DLLEXPORT void STDCALL FPDF_ClosePage(FPDF_PAGE page) {
  if (!page)
    return;

  CPDFXFA_Page* pPage = (CPDFXFA_Page*)page;
  pPage->Release();
}

DLLEXPORT void STDCALL FPDF_CloseDocument(FPDF_DOCUMENT document) {
  if (!document)
    return;

  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)document;
  delete pDoc;
}

DLLEXPORT unsigned long STDCALL FPDF_GetLastError() {
  return GetLastError();
}

DLLEXPORT void STDCALL FPDF_DeviceToPage(FPDF_PAGE page,
                                         int start_x,
                                         int start_y,
                                         int size_x,
                                         int size_y,
                                         int rotate,
                                         int device_x,
                                         int device_y,
                                         double* page_x,
                                         double* page_y) {
  if (page == NULL || page_x == NULL || page_y == NULL)
    return;
  CPDFXFA_Page* pPage = (CPDFXFA_Page*)page;

  pPage->DeviceToPage(start_x, start_y, size_x, size_y, rotate, device_x,
                      device_y, page_x, page_y);
}

DLLEXPORT void STDCALL FPDF_PageToDevice(FPDF_PAGE page,
                                         int start_x,
                                         int start_y,
                                         int size_x,
                                         int size_y,
                                         int rotate,
                                         double page_x,
                                         double page_y,
                                         int* device_x,
                                         int* device_y) {
  if (page == NULL || device_x == NULL || device_y == NULL)
    return;
  CPDFXFA_Page* pPage = (CPDFXFA_Page*)page;
  pPage->PageToDevice(start_x, start_y, size_x, size_y, rotate, page_x, page_y,
                      device_x, device_y);
}

DLLEXPORT FPDF_BITMAP STDCALL FPDFBitmap_Create(int width,
                                                int height,
                                                int alpha) {
  nonstd::unique_ptr<CFX_DIBitmap> pBitmap(new CFX_DIBitmap);
  if (!pBitmap->Create(width, height, alpha ? FXDIB_Argb : FXDIB_Rgb32)) {
    return NULL;
  }
  return pBitmap.release();
}

DLLEXPORT FPDF_BITMAP STDCALL FPDFBitmap_CreateEx(int width,
                                                  int height,
                                                  int format,
                                                  void* first_scan,
                                                  int stride) {
  FXDIB_Format fx_format;
  switch (format) {
    case FPDFBitmap_Gray:
      fx_format = FXDIB_8bppRgb;
      break;
    case FPDFBitmap_BGR:
      fx_format = FXDIB_Rgb;
      break;
    case FPDFBitmap_BGRx:
      fx_format = FXDIB_Rgb32;
      break;
    case FPDFBitmap_BGRA:
      fx_format = FXDIB_Argb;
      break;
    default:
      return NULL;
  }
  CFX_DIBitmap* pBitmap = new CFX_DIBitmap;
  pBitmap->Create(width, height, fx_format, (uint8_t*)first_scan, stride);
  return pBitmap;
}

DLLEXPORT void STDCALL FPDFBitmap_FillRect(FPDF_BITMAP bitmap,
                                           int left,
                                           int top,
                                           int width,
                                           int height,
                                           FPDF_DWORD color) {
  if (bitmap == NULL)
    return;
#ifdef _SKIA_SUPPORT_
  CFX_SkiaDevice device;
#else
  CFX_FxgeDevice device;
#endif
  device.Attach((CFX_DIBitmap*)bitmap);
  if (!((CFX_DIBitmap*)bitmap)->HasAlpha())
    color |= 0xFF000000;
  FX_RECT rect(left, top, left + width, top + height);
  device.FillRect(&rect, color);
}

DLLEXPORT void* STDCALL FPDFBitmap_GetBuffer(FPDF_BITMAP bitmap) {
  if (bitmap == NULL)
    return NULL;
  return ((CFX_DIBitmap*)bitmap)->GetBuffer();
}

DLLEXPORT int STDCALL FPDFBitmap_GetWidth(FPDF_BITMAP bitmap) {
  if (bitmap == NULL)
    return 0;
  return ((CFX_DIBitmap*)bitmap)->GetWidth();
}

DLLEXPORT int STDCALL FPDFBitmap_GetHeight(FPDF_BITMAP bitmap) {
  if (bitmap == NULL)
    return 0;
  return ((CFX_DIBitmap*)bitmap)->GetHeight();
}

DLLEXPORT int STDCALL FPDFBitmap_GetStride(FPDF_BITMAP bitmap) {
  if (bitmap == NULL)
    return 0;
  return ((CFX_DIBitmap*)bitmap)->GetPitch();
}

DLLEXPORT void STDCALL FPDFBitmap_Destroy(FPDF_BITMAP bitmap) {
  delete (CFX_DIBitmap*)bitmap;
}

void FPDF_RenderPage_Retail(CRenderContext* pContext,
                            FPDF_PAGE page,
                            int start_x,
                            int start_y,
                            int size_x,
                            int size_y,
                            int rotate,
                            int flags,
                            FX_BOOL bNeedToRestore,
                            IFSDK_PAUSE_Adapter* pause) {
  CPDF_Page* pPage = ((CPDFXFA_Page*)page)->GetPDFPage();
  if (pPage == NULL)
    return;

  if (!pContext->m_pOptions)
    pContext->m_pOptions = new CPDF_RenderOptions;

  if (flags & FPDF_LCD_TEXT)
    pContext->m_pOptions->m_Flags |= RENDER_CLEARTYPE;
  else
    pContext->m_pOptions->m_Flags &= ~RENDER_CLEARTYPE;
  if (flags & FPDF_NO_NATIVETEXT)
    pContext->m_pOptions->m_Flags |= RENDER_NO_NATIVETEXT;
  if (flags & FPDF_RENDER_LIMITEDIMAGECACHE)
    pContext->m_pOptions->m_Flags |= RENDER_LIMITEDIMAGECACHE;
  if (flags & FPDF_RENDER_FORCEHALFTONE)
    pContext->m_pOptions->m_Flags |= RENDER_FORCE_HALFTONE;
  // Grayscale output
  if (flags & FPDF_GRAYSCALE) {
    pContext->m_pOptions->m_ColorMode = RENDER_COLOR_GRAY;
    pContext->m_pOptions->m_ForeColor = 0;
    pContext->m_pOptions->m_BackColor = 0xffffff;
  }
  const CPDF_OCContext::UsageType usage =
      (flags & FPDF_PRINTING) ? CPDF_OCContext::Print : CPDF_OCContext::View;
  pContext->m_pOptions->m_AddFlags = flags >> 8;
  pContext->m_pOptions->m_pOCContext =
      new CPDF_OCContext(pPage->m_pDocument, usage);

  CFX_AffineMatrix matrix;
  pPage->GetDisplayMatrix(matrix, start_x, start_y, size_x, size_y, rotate);

  FX_RECT clip;
  clip.left = start_x;
  clip.right = start_x + size_x;
  clip.top = start_y;
  clip.bottom = start_y + size_y;
  pContext->m_pDevice->SaveState();
  pContext->m_pDevice->SetClip_Rect(&clip);

  pContext->m_pContext = new CPDF_RenderContext;
  pContext->m_pContext->Create(pPage);
  pContext->m_pContext->AppendObjectList(pPage, &matrix);

  if (flags & FPDF_ANNOT) {
    pContext->m_pAnnots = new CPDF_AnnotList(pPage);
    FX_BOOL bPrinting = pContext->m_pDevice->GetDeviceClass() != FXDC_DISPLAY;
    pContext->m_pAnnots->DisplayAnnots(pPage, pContext->m_pContext, bPrinting,
                                       &matrix, TRUE, NULL);
  }

  pContext->m_pRenderer = new CPDF_ProgressiveRenderer;
  pContext->m_pRenderer->Start(pContext->m_pContext, pContext->m_pDevice,
                               pContext->m_pOptions, pause);
  if (bNeedToRestore) {
    pContext->m_pDevice->RestoreState();
  }

  //#endif
}

DLLEXPORT int STDCALL FPDF_GetPageSizeByIndex(FPDF_DOCUMENT document,
                                              int page_index,
                                              double* width,
                                              double* height) {
  // 	CPDF_Document* pDoc = (CPDF_Document*)document;
  // 	if(pDoc == NULL)
  // 		return FALSE;
  //
  // 	CPDF_Dictionary* pDict = pDoc->GetPage(page_index);
  // 	if (pDict == NULL) return FALSE;
  //
  // 	CPDF_Page page;
  // 	page.Load(pDoc, pDict);
  // 	*width = page.GetPageWidth();
  // 	*height = page.GetPageHeight();

  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)document;
  if (pDoc == NULL)
    return FALSE;

  int count = pDoc->GetPageCount();
  if (page_index < 0 || page_index >= count)
    return FALSE;

  CPDFXFA_Page* pPage = pDoc->GetPage(page_index);
  if (!pPage)
    return FALSE;

  *width = pPage->GetPageWidth();
  *height = pPage->GetPageHeight();

  return TRUE;
}

DLLEXPORT FPDF_BOOL STDCALL
FPDF_VIEWERREF_GetPrintScaling(FPDF_DOCUMENT document) {
  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)document;
  if (!pDoc)
    return TRUE;
  CPDF_Document* pPDFDoc = pDoc->GetPDFDoc();
  if (!pPDFDoc)
    return TRUE;
  CPDF_ViewerPreferences viewRef(pPDFDoc);
  return viewRef.PrintScaling();
}

DLLEXPORT int STDCALL FPDF_VIEWERREF_GetNumCopies(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = ((CPDFXFA_Document*)document)->GetPDFDoc();
  if (!pDoc)
    return 1;
  CPDF_ViewerPreferences viewRef(pDoc);
  return viewRef.NumCopies();
}

DLLEXPORT FPDF_PAGERANGE STDCALL
FPDF_VIEWERREF_GetPrintPageRange(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = ((CPDFXFA_Document*)document)->GetPDFDoc();
  if (!pDoc)
    return NULL;
  CPDF_ViewerPreferences viewRef(pDoc);
  return viewRef.PrintPageRange();
}

DLLEXPORT FPDF_DUPLEXTYPE STDCALL
FPDF_VIEWERREF_GetDuplex(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = ((CPDFXFA_Document*)document)->GetPDFDoc();
  if (!pDoc)
    return DuplexUndefined;
  CPDF_ViewerPreferences viewRef(pDoc);
  CFX_ByteString duplex = viewRef.Duplex();
  if (FX_BSTRC("Simplex") == duplex)
    return Simplex;
  if (FX_BSTRC("DuplexFlipShortEdge") == duplex)
    return DuplexFlipShortEdge;
  if (FX_BSTRC("DuplexFlipLongEdge") == duplex)
    return DuplexFlipLongEdge;
  return DuplexUndefined;
}

DLLEXPORT FPDF_DWORD STDCALL FPDF_CountNamedDests(FPDF_DOCUMENT document) {
  if (!document)
    return 0;
  CPDF_Document* pDoc = ((CPDFXFA_Document*)document)->GetPDFDoc();

  CPDF_Dictionary* pRoot = pDoc->GetRoot();
  if (!pRoot)
    return 0;

  CPDF_NameTree nameTree(pDoc, FX_BSTRC("Dests"));
  int count = nameTree.GetCount();
  CPDF_Dictionary* pDest = pRoot->GetDict(FX_BSTRC("Dests"));
  if (pDest)
    count += pDest->GetCount();
  return count;
}

DLLEXPORT FPDF_DEST STDCALL FPDF_GetNamedDestByName(FPDF_DOCUMENT document,
                                                    FPDF_BYTESTRING name) {
  if (!document)
    return NULL;
  if (!name || name[0] == 0)
    return NULL;

  CPDFXFA_Document* pDoc = (CPDFXFA_Document*)document;
  CPDF_Document* pPDFDoc = pDoc->GetPDFDoc();
  if (!pPDFDoc)
    return NULL;
  CPDF_NameTree name_tree(pPDFDoc, FX_BSTRC("Dests"));
  return name_tree.LookupNamedDest(pPDFDoc, name);
}

FPDF_RESULT FPDF_BStr_Init(FPDF_BSTR* str) {
  if (!str)
    return -1;

  FXSYS_memset(str, 0, sizeof(FPDF_BSTR));
  return 0;
}

FPDF_RESULT FPDF_BStr_Set(FPDF_BSTR* str, FPDF_LPCSTR bstr, int length) {
  if (!str)
    return -1;
  if (!bstr || !length)
    return -1;
  if (length == -1)
    length = FXSYS_strlen(bstr);

  if (length == 0) {
    if (str->str) {
      FX_Free(str->str);
      str->str = NULL;
    }
    str->len = 0;
    return 0;
  }

  if (str->str && str->len < length)
    str->str = FX_Realloc(char, str->str, length + 1);
  else if (!str->str)
    str->str = FX_Alloc(char, length + 1);

  str->str[length] = 0;
  if (str->str == NULL)
    return -1;

  FXSYS_memcpy(str->str, bstr, length);
  str->len = length;

  return 0;
}

FPDF_RESULT FPDF_BStr_Clear(FPDF_BSTR* str) {
  if (!str)
    return -1;

  if (str->str) {
    FX_Free(str->str);
    str->str = NULL;
  }
  str->len = 0;
  return 0;
}

DLLEXPORT FPDF_DEST STDCALL FPDF_GetNamedDest(FPDF_DOCUMENT document,
                                              int index,
                                              void* buffer,
                                              long* buflen) {
  if (!buffer)
    *buflen = 0;
  if (!document || index < 0)
    return NULL;
  CPDF_Document* pDoc = ((CPDFXFA_Document*)document)->GetPDFDoc();

  CPDF_Dictionary* pRoot = pDoc->GetRoot();
  if (!pRoot)
    return NULL;

  CPDF_Object* pDestObj = NULL;
  CFX_ByteString bsName;
  CPDF_NameTree nameTree(pDoc, FX_BSTRC("Dests"));
  int count = nameTree.GetCount();
  if (index >= count) {
    CPDF_Dictionary* pDest = pRoot->GetDict(FX_BSTRC("Dests"));
    if (!pDest)
      return NULL;
    if (index >= count + pDest->GetCount())
      return NULL;
    index -= count;
    FX_POSITION pos = pDest->GetStartPos();
    int i = 0;
    while (pos) {
      pDestObj = pDest->GetNextElement(pos, bsName);
      if (!pDestObj)
        continue;
      if (i == index)
        break;
      i++;
    }
  } else {
    pDestObj = nameTree.LookupValue(index, bsName);
  }
  if (!pDestObj)
    return NULL;
  if (pDestObj->GetType() == PDFOBJ_DICTIONARY) {
    pDestObj = ((CPDF_Dictionary*)pDestObj)->GetArray(FX_BSTRC("D"));
    if (!pDestObj)
      return NULL;
  }
  if (pDestObj->GetType() != PDFOBJ_ARRAY)
    return NULL;
  CFX_WideString wsName = PDF_DecodeText(bsName);
  CFX_ByteString utf16Name = wsName.UTF16LE_Encode();
  unsigned int len = utf16Name.GetLength();
  if (!buffer) {
    *buflen = len;
  } else if (*buflen >= len) {
    memcpy(buffer, utf16Name.c_str(), len);
    *buflen = len;
  } else {
    *buflen = -1;
  }
  return (FPDF_DEST)pDestObj;
}
