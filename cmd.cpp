#include "stdafx.h"
#include "resource.h"

_NT_BEGIN

class __declspec(novtable) SIoObj
{
protected:
	HANDLE _hFile = 0;
	virtual ~SIoObj()
	{
		CloseObjectHandle();
	}
private:
	friend class SIRP;
	virtual void IOCompletionRoutine(ULONG Code, ULONG dwErrorCode, ULONG dwNumberOfBytesTransfered, PVOID Ctx) = 0;
public:
	virtual void AddRef() = 0;
	virtual void Release() = 0;

	virtual void CloseObjectHandle()
	{
		if (_hFile) CloseHandle(_hFile), _hFile = 0;
	}
};

class SIRP : public OVERLAPPED 
{
	SIoObj* pObj;
	PVOID Pointer;
	ULONG op;
	PVOID buf[];

	~SIRP()
	{
		pObj->Release();
	}

	VOID IoCompleted(_In_ DWORD dwErrorCode, _In_ DWORD dwNumberOfBytesTransfered)
	{
		pObj->IOCompletionRoutine(op, dwErrorCode, dwNumberOfBytesTransfered, Pointer);
		delete this;
	}

public:
	static VOID WINAPI sIoCompleted(
		_In_    DWORD dwErrorCode,
		_In_    DWORD dwNumberOfBytesTransfered,
		_Inout_ LPOVERLAPPED lpOverlapped
		)
	{
		static_cast<SIRP*>(lpOverlapped)->IoCompleted(dwErrorCode, dwNumberOfBytesTransfered);
	}

	void CheckIoCompleted(BOOL f)
	{
		CheckIoCompleted(f ? NOERROR : GetLastError());
	}

	void CheckIoCompleted(ULONG dwError)
	{
		switch (dwError)
		{
		case NOERROR:
		case ERROR_IO_PENDING:
			return;
		}

		IoCompleted(dwError, 0);
	}

	void* operator new(size_t s, ULONG cb = 0)
	{
		return LocalAlloc(0, s + cb);
	}

	void operator delete(void* pv)
	{
		LocalFree(pv);
	}

	PVOID GetBuf()
	{
		Pointer = buf;
		return buf;
	}

	SIRP(SIoObj* pObj, ULONG op, PVOID Pointer) : op(op), pObj(pObj), Pointer(Pointer)
	{
		RtlZeroMemory(static_cast<OVERLAPPED*>(this), sizeof(OVERLAPPED));
		pObj->AddRef();
	}
};

void ShowError(HWND hwnd, ULONG dwError)
{
	WCHAR buf[0x100];
	if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS, 
		0, dwError, 0, buf, _countof(buf), 0))
	{
		MessageBoxW(hwnd, buf, L"Fail start Cmd!", MB_ICONHAND);
	}
}

class SDialog : public SIoObj
{
	enum { opRead, opWrite, buf_size = 0x1000 };

	HANDLE _hProcess = 0;
	HFONT _hFont;
	HWND _hwndOut, _hwndCmd, _hwndDlg, _hwndExec;
	HICON _hi[2]={};
	PSTR _buf = 0;

	INT_PTR OnInitDialog(HWND hwndDlg);

	ULONG StartCmd();
	void OnOk(HWND hwndDlg);

	// all in single thread, here not need
	virtual void AddRef()
	{
	}

	virtual void Release()
	{
	}

	virtual void IOCompletionRoutine(ULONG Code, ULONG dwErrorCode, ULONG dwNumberOfBytesTransfered, PVOID Ctx)
	{
		switch (dwErrorCode)
		{
		default:
			ShowError(_hwndDlg, dwErrorCode);
		case ERROR_BROKEN_PIPE:
		case ERROR_OPERATION_ABORTED:
			CloseObjectHandle();
			EnableWindow(_hwndCmd, FALSE);
			break;
		case NOERROR:
			break;
		}

		switch (Code)
		{
		case opRead:
			if (dwErrorCode == NOERROR)
			{
				if (dwNumberOfBytesTransfered)
				{
					OnRead((PCSTR)Ctx, dwNumberOfBytesTransfered);
				}

				if (_hFile)
				{
					Read();
				}
			}
			break;
		case opWrite:
			break;
		default: __debugbreak();
		}
	}

	void OnRead(PCSTR buf, ULONG cb)
	{
		PWSTR psz = 0;
		ULONG cch = 0;
		while (cch = MultiByteToWideChar(CP_OEMCP, 0, buf, cb, psz, cch))
		{
			if (psz)
			{
				psz[cch] = 0;
				SendMessageW(_hwndOut, EM_SETSEL, MAXLONG, MAXLONG);
				SendMessageW(_hwndOut, EM_REPLACESEL, FALSE, (LPARAM)psz);
				break;
			}

			psz = (PWSTR)alloca((cch + 1) * sizeof(WCHAR));
		}
	}

	void OnDestroy()
	{
		CloseObjectHandle();
		if (_hProcess) CloseHandle(_hProcess), _hProcess = 0;
		SleepEx(0, TRUE); // ZwTestAlert();
		if (_buf) delete [] _buf;
	}

	INT_PTR DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);

	static INT_PTR CALLBACK _DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
	{
		return reinterpret_cast<SDialog*>(GetWindowLongPtrW(hwndDlg, DWLP_USER))->DialogProc(hwndDlg, uMsg, wParam, lParam);
	}

	static INT_PTR CALLBACK StartDialogProc(HWND hwndDlg, UINT uMsg, WPARAM /*wParam*/, LPARAM lParam)
	{
		if (uMsg == WM_INITDIALOG)
		{
			SetWindowLongPtrW(hwndDlg, DWLP_USER, lParam);
			SetWindowLongPtrW(hwndDlg, DWLP_DLGPROC, (LONG_PTR)_DialogProc);
			return reinterpret_cast<SDialog*>(lParam)->OnInitDialog(hwndDlg);
		}

		return 0;
	}

	void Read()
	{
		if (SIRP* irp = new SIRP(this, opRead, _buf))
		{
			irp->CheckIoCompleted(ReadFileEx(_hFile, _buf, buf_size, irp, SIRP::sIoCompleted));
		}
	}

	void SendCmd()
	{
		if (ULONG len = GetWindowTextLengthW(_hwndCmd))
		{
			PWSTR psz = (PWSTR)alloca(++len * sizeof(WCHAR));

			len = GetWindowTextW(_hwndCmd, psz, len);

			PSTR buf = 0;
			ULONG cb = 0;
			SIRP* irp = 0;
			
			while (cb = WideCharToMultiByte(CP_OEMCP, 0, psz, len, buf, cb, 0, 0))
			{
				if (irp)
				{
					buf[cb] = '\r', buf[cb+1] = '\n';

					irp->CheckIoCompleted(WriteFileEx(_hFile, buf, cb + 2, irp, SIRP::sIoCompleted));
					return;
				}

				if (irp = new(cb + 2) SIRP(this, opWrite, 0))
				{
					buf = (PSTR)irp->GetBuf();
				}
				else
				{
					return ;
				}
			}
		}
	}

public:

	HWND Create()
	{
		return CreateDialogParamW((HINSTANCE)&__ImageBase, MAKEINTRESOURCEW(IDD_DIALOG1), HWND_DESKTOP, StartDialogProc, (LPARAM)this);
	}

	void Run(HWND hwnd);
};

ULONG CreatePipePair(PHANDLE ServerPipe, PHANDLE ClientPipe)
{
	WCHAR Name[32];

	swprintf_s(Name, _countof(Name), L"\\\\?\\pipe\\%016I64x", ~GetTickCount64() + GetCurrentProcessId());

	HANDLE hPipe = CreateNamedPipeW(Name,
		PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 0, 0, 0, 0);

	if (hPipe != INVALID_HANDLE_VALUE)
	{
		SECURITY_ATTRIBUTES sa = { sizeof(sa), 0, TRUE };

		HANDLE hFile = CreateFileW(Name, FILE_GENERIC_READ | FILE_GENERIC_WRITE,
			0, &sa, OPEN_EXISTING, 0, 0);

		if (hFile != INVALID_HANDLE_VALUE)
		{
			*ClientPipe = hFile, *ServerPipe = hPipe;
			return NOERROR;
		}

		CloseHandle(hPipe);
	}

	return GetLastError();
}

ULONG SDialog::StartCmd()
{
	WCHAR lpApplicationName[MAX_PATH];

	ULONG dwError = BOOL_TO_ERROR(GetEnvironmentVariableW(L"ComSpec", lpApplicationName, _countof(lpApplicationName)));

	if (dwError == NOERROR)
	{
		PROCESS_INFORMATION pi;
		STARTUPINFOEXW si = { { sizeof(si)} };

		SIZE_T s = 0;
__loop:
		switch (dwError = BOOL_TO_ERROR(InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &s)))
		{
		case NOERROR:
			if (si.lpAttributeList)
			{
				if (NOERROR == (dwError = BOOL_TO_ERROR(UpdateProcThreadAttribute(si.lpAttributeList,
					0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &si.StartupInfo.hStdError, sizeof(HANDLE), 0, 0))) &&
					NOERROR == (dwError = CreatePipePair(&_hFile, &si.StartupInfo.hStdError)))
				{
					si.StartupInfo.hStdInput = si.StartupInfo.hStdError;
					si.StartupInfo.hStdOutput = si.StartupInfo.hStdError;
					si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

					dwError = BOOL_TO_ERROR(CreateProcessW(lpApplicationName, 0, 0, 0, TRUE,
						CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, 0, 0, &si.StartupInfo, &pi));

					CloseHandle(si.StartupInfo.hStdError);

					if (dwError == NOERROR)
					{
						CloseHandle(pi.hThread);
						_hProcess = pi.hProcess;
						EnableWindow(_hwndExec, FALSE);
						EnableWindow(_hwndCmd, TRUE);
						Read();
						return NOERROR;
					}

					CloseObjectHandle();
				}

				return dwError;
			}

			return ERROR_GEN_FAILURE;

		case ERROR_INSUFFICIENT_BUFFER:
			if (si.lpAttributeList)
			{
		default:
			break;
			}
			si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)alloca(s);
			goto __loop;
		}
	}
	return dwError;
}

void SDialog::OnOk(HWND hwndDlg)
{
	if (ULONG dwError = StartCmd())
	{
		ShowError(hwndDlg, dwError);
	}
}

INT_PTR SDialog::OnInitDialog(HWND hwndDlg)
{
	if (_buf = new char[buf_size])
	{
		_hwndDlg = hwndDlg;
		_hwndCmd = GetDlgItem(hwndDlg, IDC_EDIT1);
		_hwndOut = GetDlgItem(hwndDlg, IDC_EDIT2);
		_hwndExec = GetDlgItem(hwndDlg, IDC_BUTTON1);

		SendMessageW(_hwndOut, EM_LIMITTEXT, MAXLONG, 0);

		static const int 
			X_index[] = { SM_CXSMICON, SM_CXICON }, 
			Y_index[] = { SM_CYSMICON, SM_CYICON },
			icon_type[] = { ICON_SMALL, ICON_BIG};

		ULONG i = _countof(icon_type) - 1;
		do 
		{
			HICON hi;

			if (0 <= LoadIconWithScaleDown((HINSTANCE)&__ImageBase, MAKEINTRESOURCE(IDI_ICON1), 
				GetSystemMetrics(X_index[i]), GetSystemMetrics(Y_index[i]), &hi))
			{
				_hi[i] = hi;
				SendMessage(hwndDlg, WM_SETICON, icon_type[i], (LPARAM)hi);
			}
		} while (i--);
	}
	else
	{
		DestroyWindow(hwndDlg);
	}

	return 0;
}

INT_PTR SDialog::DialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM /*lParam*/)
{
	switch (uMsg)
	{
	case WM_CTLCOLOREDIT:
		SetTextColor((HDC)wParam, RGB(0,255,0));
		SetBkColor((HDC)wParam, RGB(0,0,0));
		return (LPARAM)GetStockObject(BLACK_BRUSH);
	case WM_NCDESTROY:
		PostQuitMessage(0);
		break;
	case WM_DESTROY:
		OnDestroy();
		break;
	case WM_COMMAND:
		switch (wParam)
		{
		case IDC_BUTTON1:
			OnOk(hwndDlg);
			break;
		case IDOK:
			SendCmd();
			break;
		case IDCANCEL:
			DestroyWindow(hwndDlg);
			break;
		}
		break;
	}
	return 0;
}

void SDialog::Run(HWND hwnd)
{
	for (;;)
	{
		ULONG nCount = _hProcess ? 1 : 0;

		ULONG r = MsgWaitForMultipleObjectsEx(nCount, &_hProcess, INFINITE, QS_ALLINPUT, MWMO_ALERTABLE|MWMO_INPUTAVAILABLE);

		if (r == nCount)
		{
			MSG msg;

			while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT) 
				{
					return ;
				}

				if (!IsDialogMessage(hwnd, &msg))
				{
					if (msg.message - WM_KEYFIRST <= WM_KEYLAST - WM_KEYFIRST)
					{
						TranslateMessage(&msg);
					}
					DispatchMessage(&msg);
				}
			}

			continue;
		}

		if (r < nCount)
		{
			CloseHandle(_hProcess), _hProcess = 0;
			CloseObjectHandle();
			EnableWindow(_hwndCmd, FALSE);
			EnableWindow(_hwndExec, TRUE);
			continue;
		}

		if (r == WAIT_FAILED)
		{
			return;
		}
	}
}

void WINAPI eP(void*)
{
	{
		SDialog dlg;
		if (HWND hwnd = dlg.Create())
		{
			ShowWindow(hwnd, SW_SHOW);
			dlg.Run(hwnd);
		}
	}

	ExitProcess(0);
}

_NT_END