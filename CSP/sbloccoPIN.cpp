#include "../stdafx.h"
#include "IAS.h"
#include "CSP.h"
#include "../util/ModuleInfo.h"
#include "../UI/Message.h"
#include "../UI/Pin.h"
#include <functional>
#include "../crypto/ASNParser.h"
#include "Cardmod.h"
#include "../UI/SystemTray.h"
#include "../UI/safeDesktop.h"
#include "../PCSC/PCSC.h"
#include <atlbase.h>
#include <shlobj_core.h>

extern CModuleInfo moduleInfo;
extern "C" DWORD WINAPI CardAcquireContext(IN PCARD_DATA pCardData, __in DWORD dwFlags);

#ifdef _WIN64
#pragma comment(linker, "/export:SbloccoPIN")
#else
#pragma comment(linker, "/export:SbloccoPIN=_SbloccoPIN@16")
#endif

DWORD WINAPI _sbloccoPIN(
	DWORD threadId) {
	init_func

		try {
		DWORD len = 0;
		ByteDynArray IdServizi;
		std::unique_ptr<safeDesktop> desk;

		SCARDCONTEXT hSC;

		SCardEstablishContext(SCARD_SCOPE_SYSTEM, nullptr, nullptr, &hSC);
		char *readers = nullptr;
		len = SCARD_AUTOALLOCATE;
		if (SCardListReaders(hSC, nullptr, (char*)&readers, &len) != SCARD_S_SUCCESS || readers == nullptr) {
			CMessage msg(MB_OK,
				"Sblocco PIN",
				"Nessun lettore di smartcard installato");
			msg.DoModal();
			return 0;
		}
		char *curreader = readers;
		bool foundCIE = false, isEnrolled = false;
		for (; curreader[0] != 0; curreader += strnlen(curreader, len) + 1) {
			CARD_DATA cData;
			ZeroMem(cData);
			cData.dwVersion = 7;
			cData.hSCardCtx = hSC;
			{
				safeConnection conn(hSC, curreader, SCARD_SHARE_SHARED);
				if (conn.hCard == NULL)
					continue;


				{
					safeTransaction checkTran(conn, SCARD_LEAVE_CARD);
					if (!checkTran.isLocked())
						continue;

					len = SCARD_AUTOALLOCATE;
					cData.hScard = conn;
					SCardGetAttrib(cData.hScard, SCARD_ATTR_ATR_STRING, (BYTE*)&cData.pbAtr, &len);
					cData.pfnCspAlloc = (PFN_CSP_ALLOC)CryptMemAlloc;
					cData.pfnCspReAlloc = (PFN_CSP_REALLOC)CryptMemRealloc;
					cData.pfnCspFree = (PFN_CSP_FREE)CryptMemFree;
					cData.cbAtr = len;
					cData.pwszCardName = L"CIE";
					auto isCIE = CardAcquireContext(&cData, 0);
					SCardFreeMemory(cData.hScard, cData.pbAtr);
					isEnrolled = (((IAS*)cData.pvVendorSpecific)->IsEnrolled());						
					if (isCIE != 0)
						continue;
				}
				foundCIE = true;
				if (!desk)
					desk.reset(new safeDesktop("AbilitaCIE"));

				CPin puk(8, "Inserire le 8 cifre del PUK della CIE", "", "", "Sblocco PIN");
				if (puk.DoModal() == IDOK) {
					int numCifre = 8;
					std::string msg;
					msg = "Inserire le 8 cifre del nuovo PIN";

					CPin newPin(numCifre, msg.c_str(), "", "", "Sblocco PIN", true);
					if (newPin.DoModal() == IDOK) {
						try {

							safeTransaction Tran(conn, SCARD_LEAVE_CARD);
							if (!Tran.isLocked())
								continue;

							len = 0;
							auto ias = ((IAS*)cData.pvVendorSpecific);

							auto ris = CardUnblockPin(&cData, wszCARD_USER_USER, (BYTE*)puk.PIN, (DWORD)strnlen(puk.PIN, sizeof(puk.PIN)), (BYTE*)newPin.PIN, (DWORD)strnlen(newPin.PIN, sizeof(newPin.PIN)), 0, CARD_AUTHENTICATE_PIN_PIN);
							if (ris == SCARD_W_WRONG_CHV) {
								std::string num;
								if (ias->attemptsRemaining >= 0)
									num = "PUK errato. Sono rimasti " + std::to_string(ias->attemptsRemaining) + " tentativi";
								else
									num = "";
								CMessage msg(MB_OK, "Sblocco PIN",									
									num.c_str(),
									"prima di bloccare il PUK");
								msg.DoModal();
								if (threadId != 0)
									PostThreadMessage(threadId, WM_COMMAND, 1, 0);

								break;
							}
							else if (ris == SCARD_W_CHV_BLOCKED) {
								CMessage msg(MB_OK,
									"Sblocco PIN",
									"Il PUK � bloccato. La CIE non pu� pi� essere sbloccata");
								msg.DoModal();
								if (threadId != 0)
									PostThreadMessage(threadId, WM_COMMAND, 0, 0);
								break;
							}
							else if (ris != 0)
								throw logged_error("Autenticazione fallita");

							CMessage msg(MB_OK, "Sblocco PIN",
								"Il PIN � stato sbloccato correttamente");
							msg.DoModal();
							if (threadId != 0)
								PostThreadMessage(threadId, WM_COMMAND, 0, 0);

						}
						catch (std::exception &ex) {
							std::string dump;
							OutputDebugString(ex.what());
							CMessage msg(MB_OK, "Sblocco PIN",
								"Si � verificato un errore nella verifica del PUK");
							msg.DoModal();
							if (threadId != 0)
								PostThreadMessage(threadId, WM_COMMAND, 0, 0);
							break;
						}
					}
					else
						if (threadId != 0)
							PostThreadMessage(threadId, WM_COMMAND, 1, 0);
				}
				else
					if (threadId != 0)
						PostThreadMessage(threadId, WM_COMMAND, 1, 0);
				break;
			}
		}
		if (!foundCIE) {
			if (!desk)
				desk.reset(new safeDesktop("AbilitaCIE"));
			CMessage msg(MB_OK, "Sblocco PIN",
				"Impossibile trovare una CIE",
				"nei lettori di smart card");
			msg.DoModal();
			if (threadId != 0)
				PostThreadMessage(threadId, WM_COMMAND, 0, 0);
		}
		SCardFreeMemory(hSC, readers);
	}
	catch (std::exception &ex) {		
		MessageBox(nullptr, "Si � verificato un errore nella verifica di autenticit� del documento", "CIE", MB_OK);
	}

	return 0;
	exit_func
	return E_UNEXPECTED;
}

void TrayNotification(CSystemTray* tray, WPARAM uID, LPARAM lEvent) {
	if (lEvent == WM_LBUTTONUP || lEvent== 0x405) {

		tray->HideIcon();
		PROCESS_INFORMATION pi;
		STARTUPINFO si;
		ZeroMem(si);
		si.cb = sizeof(STARTUPINFO);

		char szProgramFilesDir[MAX_PATH];
		if (!SHGetSpecialFolderPath(NULL, szProgramFilesDir, CSIDL_PROGRAM_FILESX86, 0))
			SHGetSpecialFolderPath(NULL, szProgramFilesDir, CSIDL_PROGRAM_FILES, 0);

		Log.writePure("szProgramFilesDir %s", szProgramFilesDir);

		if (!CreateProcess(NULL, (char*)std::string(szProgramFilesDir).append("\\CIEPKI\\CIEID ").append("unlock").c_str(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
		{
			DWORD dwerr = GetLastError();
			Log.writePure("error run CIEID %x", dwerr);
			throw logged_error("Errore in creazione processo CIEID");
		}
		else
		{
			throw logged_error("Errore in creazione processo CIEID");
		}

		//std::thread thread(_sbloccoPIN, GetCurrentThreadId());
		//thread.detach();
		//tray->HideIcon();
	}
}

extern "C" int CALLBACK SbloccoPIN(
	_In_ HINSTANCE hInstance,
	_In_ HINSTANCE hPrevInstance,
	_In_ LPSTR     lpCmdLine,
	_In_ int       nCmdShow
	)
{
	init_CSP_func
	if (_AtlWinModule.cbSize != sizeof(_ATL_WIN_MODULE)) {
		_AtlWinModule.cbSize = sizeof(_ATL_WIN_MODULE);
		AtlWinModuleInit(&_AtlWinModule);
	}

	WNDCLASS wndClass;
	GetClassInfo(NULL, WC_DIALOG, &wndClass);
	wndClass.hInstance = (HINSTANCE)moduleInfo.getModule();
	wndClass.style |= CS_DROPSHADOW;
	wndClass.lpszClassName = "CIEDialog";
	RegisterClass(&wndClass);

	ODS("Start SbloccoPIN");
	if (!CheckOneInstance("CIESbloccoOnce")) {
		ODS("Already running SbloccoPIN");
		return 0;
	}	

	if (strcmp(lpCmdLine, "ICON") == 0) {

		ODS("Start SbloccoPIN with ICON");

		CSystemTray tray(wndClass.hInstance, nullptr, WM_APP, "Premere per sbloccare il PIN della CIE",
			LoadIcon(wndClass.hInstance, MAKEINTRESOURCE(IDI_CIE)), 1);
		tray.ShowBalloon("Premere per sbloccare il PIN dalla CIE", "CIE", NIIF_INFO);
		tray.ShowIcon();
		tray.TrayNotification = TrayNotification;
		MSG Msg;
		while (GetMessage(&Msg, NULL, 0, 0) > 0)
		{
			TranslateMessage(&Msg);
			if (Msg.message == WM_COMMAND) {
				ODS("WMCOMMAND");
				if (Msg.wParam == 0)
					return 0;
				else {
					tray.ShowIcon();
					ODS("Show Baloon");
					tray.ShowBalloon("Premere per sbloccare il PIN dalla CIE", "CIE", NIIF_INFO);
				}
			}
			DispatchMessage(&Msg);
		}
	}
	else {
		//std::thread thread(_sbloccoPIN, 0);
		//thread.join();
		_sbloccoPIN(0);

		ODS("End SbloccoPIN");
		return 0;
	}
	exit_CSP_func
	return 0;
}
