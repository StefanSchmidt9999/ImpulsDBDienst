#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <mq.h>
#include <memory>
#include <oledb.h>

#include <sqloledb.h>
#include <comdef.h>
#include <atlbase.h>

#include <string>
#include <fstream>
#include <vector>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "OleAut32.lib")

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Mqrt.lib")

#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

#include <string_view>

#include <oledberr.h>
#include <msdaguid.h>


// Undefine GetCurrentTime macro to prevent
// conflict with Storyboard::GetCurrentTime
#undef GetCurrentTime


#define SERVICE_NAME L"ImpulsDBDienst"

SERVICE_STATUS gStatus = {};
SERVICE_STATUS_HANDLE gStatusHandle = nullptr;
HANDLE gStopEvent = nullptr;

QUEUEHANDLE gDbQueue = nullptr;
QUEUEHANDLE gOutQueue = nullptr;
QUEUEHANDLE gErrorQueue = nullptr;

struct DbParameter
{
    std::wstring Name;
    std::wstring Value;
};

std::vector<DbParameter> ExtractParameters(const std::wstring& xml);
std::wstring EscapeSql(const std::wstring& value);
std::wstring BuildSqlCommand(const std::wstring& procedureName, const std::vector<DbParameter>& parameters);

std::wstring ExecuteStoredProcedureXml(const std::wstring& procedureName, const std::vector<DbParameter>& parameters);

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void WINAPI ServiceCtrlHandler(DWORD ctrlCode);

//std::wstring ExecuteStoredProcedureXml(const std::wstring& procedureName);

void WriteLog(const std::wstring& text);

bool OpenPrivateQueue(const std::wstring& queueName, DWORD accessMode, QUEUEHANDLE* queueHandle);
bool OpenQueues();
void CloseQueues();

bool ReceiveMessageFromDb(std::wstring& xmlText);
void SendMessageToQueue( QUEUEHANDLE queueHandle,  const std::wstring& xmlText, const std::wstring& label);

std::wstring ExtractTagValue(const std::wstring& xml, const std::wstring& tagName);
std::wstring BuildStoredProcedureName(const std::wstring& commandId, const std::wstring& storedProcedureId);
std::string WStringToUtf8(const std::wstring& text);
std::wstring Utf8ToWString(const std::string& text);
// std::wstring ExtractTagValue(const std::wstring& xml, const std::wstring& tagName);

// ------------------------------------------------------------

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SERVICE_TABLE_ENTRY table[] =
    {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcher(table))
    {
        MessageBox(
            nullptr,
            L"Läuft NICHT als Service. Debugmodus.",
            SERVICE_NAME,
            MB_OK);

        ServiceMain(0, nullptr);
    }

    return 0;
}

// ------------------------------------------------------------

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    gStatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);

    gStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    gStatus.dwCurrentState = SERVICE_START_PENDING;
    gStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;

    SetServiceStatus(gStatusHandle, &gStatus);

    gStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    WriteLog(L"ImpulsDBDienst gestartet.");

    if (!OpenQueues())
    {
        WriteLog(L"DBDienst kann nicht starten, Queues fehlen.");

        gStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(gStatusHandle, &gStatus);
        return;
    }

    gStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(gStatusHandle, &gStatus);

    while (WaitForSingleObject(gStopEvent, 1000) == WAIT_TIMEOUT)
    {
        std::wstring xmlText;

        if (ReceiveMessageFromDb(xmlText))
        {
            WriteLog(L"Nachricht aus impuls_db empfangen.");

            std::wstring commandId = ExtractTagValue(xmlText, L"CommandId");

            std::wstring storedProcedureId = ExtractTagValue(xmlText, L"StoredProcedureId");
            // Test

            if (commandId.empty() || storedProcedureId.empty())
            {
                WriteLog(L"CommandId oder StoredProcedureId fehlt. Nachricht geht nach impuls_error.");

                SendMessageToQueue(gErrorQueue, xmlText, L"DB_ERROR_MISSING_COMMAND");

                continue;
            }

            std::wstring procedureName = BuildStoredProcedureName(commandId, storedProcedureId);

            WriteLog(L"CommandId: " + commandId);
            WriteLog(L"StoredProcedureId: " + storedProcedureId);
            WriteLog(L"Gebildeter Stored Procedure Name: " + procedureName);

            // std::wstring sqlXml = ExecuteStoredProcedureXml(procedureName);
            
            std::vector<DbParameter> parameters = ExtractParameters(xmlText);
            std::wstring clientId = ExtractTagValue(xmlText, L"ClientId");

            std::wstring sqlXml = ExecuteStoredProcedureXml(procedureName, parameters);
            
            std::wstring responseXml =
                L"<?xml version=\"1.0\"?>"
                L"<DBResponse>"
                L"<ClientId>" + clientId + L"</ClientId>"
                L"<Status>OK</Status>"
                L"<CommandId>" + commandId + L"</CommandId>"
                L"<StoredProcedureId>" + storedProcedureId + L"</StoredProcedureId>"
                L"<ProcedureName>" + procedureName + L"</ProcedureName>"
                L"<Data>"
                + sqlXml +
                L"</Data>"
                L"</DBResponse>";

            SendMessageToQueue(
                gOutQueue,
                responseXml,
                L"DB_RESPONSE_" + procedureName);
        }
    }

    CloseQueues();

    WriteLog(L"ImpulsDBDienst wird beendet.");

    gStatus.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(gStatusHandle, &gStatus);
}

// ------------------------------------------------------------

void WINAPI ServiceCtrlHandler(DWORD ctrlCode)
{
    if (ctrlCode == SERVICE_CONTROL_STOP)
    {
        gStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(gStatusHandle, &gStatus);

        SetEvent(gStopEvent);
    }
}

// ------------------------------------------------------------

void WriteLog(const std::wstring& text)
{
    CreateDirectory(L"C:\\Temp", nullptr);

    std::wofstream file(
        L"C:\\Temp\\ImpulsDBDienst.log",
        std::ios::app);

    if (file.is_open())
    {
        SYSTEMTIME st;
        GetLocalTime(&st);

        file << L"["
            << st.wDay << L"." << st.wMonth << L"." << st.wYear
            << L" "
            << st.wHour << L":" << st.wMinute << L":" << st.wSecond
            << L"] "
            << text
            << std::endl;
    }
}

// ------------------------------------------------------------

bool OpenPrivateQueue(
    const std::wstring& queueName,
    DWORD accessMode,
    QUEUEHANDLE* queueHandle)
{
    std::wstring pathName =
        L".\\private$\\" + queueName;

    DWORD formatNameLength = 256;
    WCHAR formatName[256]{};

    HRESULT hr = MQPathNameToFormatName(
        pathName.c_str(),
        formatName,
        &formatNameLength);

    if (FAILED(hr))
    {
        WriteLog(L"MQPathNameToFormatName fehlgeschlagen: " + pathName);
        return false;
    }

    hr = MQOpenQueue(
        formatName,
        accessMode,
        MQ_DENY_NONE,
        queueHandle);

    if (FAILED(hr))
    {
        WriteLog(L"MQOpenQueue fehlgeschlagen: " + pathName);
        return false;
    }

    WriteLog(L"Queue geöffnet: " + pathName);

    return true;
}

// ------------------------------------------------------------

bool OpenQueues()
{
    bool ok = true;

    if (!OpenPrivateQueue(
        L"impuls_db",
        MQ_RECEIVE_ACCESS,
        &gDbQueue))
    {
        ok = false;
    }

    if (!OpenPrivateQueue(
        L"impuls_out",
        MQ_SEND_ACCESS,
        &gOutQueue))
    {
        ok = false;
    }

    if (!OpenPrivateQueue(
        L"impuls_error",
        MQ_SEND_ACCESS,
        &gErrorQueue))
    {
        ok = false;
    }

    if (ok)
    {
        WriteLog(L"Alle DBDienst-Queues erfolgreich geöffnet.");
    }
    else
    {
        WriteLog(L"FEHLER: Nicht alle DBDienst-Queues konnten geöffnet werden.");
    }

    return ok;
}

// ------------------------------------------------------------

void CloseQueues()
{
    if (gDbQueue)
    {
        MQCloseQueue(gDbQueue);
        gDbQueue = nullptr;
        WriteLog(L"Queue geschlossen: impuls_db");
    }

    if (gOutQueue)
    {
        MQCloseQueue(gOutQueue);
        gOutQueue = nullptr;
        WriteLog(L"Queue geschlossen: impuls_out");
    }

    if (gErrorQueue)
    {
        MQCloseQueue(gErrorQueue);
        gErrorQueue = nullptr;
        WriteLog(L"Queue geschlossen: impuls_error");
    }
}

// ------------------------------------------------------------

bool ReceiveMessageFromDb(std::wstring& xmlText)
{
    const DWORD BODY_BUFFER_SIZE = 128 * 1024;

    std::vector<UCHAR> bodyBuffer(BODY_BUFFER_SIZE);

    MQMSGPROPS msgProps{};
    MSGPROPID propId[1]{};
    MQPROPVARIANT propVar[1]{};
    HRESULT status[1]{};

    propId[0] = PROPID_M_BODY;

    propVar[0].vt = VT_VECTOR | VT_UI1;
    propVar[0].caub.pElems = bodyBuffer.data();
    propVar[0].caub.cElems = BODY_BUFFER_SIZE;

    msgProps.cProp = 1;
    msgProps.aPropID = propId;
    msgProps.aPropVar = propVar;
    msgProps.aStatus = status;

    HRESULT hr = MQReceiveMessage(
        gDbQueue,
        1000,
        MQ_ACTION_RECEIVE,
        &msgProps,
        nullptr,
        nullptr,
        nullptr,
        MQ_NO_TRANSACTION);

    if (hr == MQ_ERROR_IO_TIMEOUT)
    {
        return false;
    }

    if (FAILED(hr))
    {
        WriteLog(L"MQReceiveMessage fehlgeschlagen.");
        return false;
    }

    DWORD bytesRead = propVar[0].caub.cElems;

    if (bytesRead == 0)
    {
        WriteLog(L"Leere Nachricht empfangen.");
        return false;
    }

    std::string utf8Text(
        reinterpret_cast<char*>(bodyBuffer.data()),
        bytesRead);

    xmlText = Utf8ToWString(utf8Text);

    if (xmlText.empty())
    {
        WriteLog(L"UTF-8 nach UTF-16 Konvertierung fehlgeschlagen.");
        return false;
    }

    return true;
}

// ------------------------------------------------------------

void SendMessageToQueue(
    QUEUEHANDLE queueHandle,
    const std::wstring& xmlText,
    const std::wstring& label)
{
    std::string utf8Text =
        WStringToUtf8(xmlText);

    if (utf8Text.empty())
    {
        WriteLog(L"UTF-16 nach UTF-8 Konvertierung fehlgeschlagen.");
        return;
    }

    MQMSGPROPS msgProps{};
    MSGPROPID propId[2]{};
    MQPROPVARIANT propVar[2]{};
    HRESULT status[2]{};

    propId[0] = PROPID_M_LABEL;
    propVar[0].vt = VT_LPWSTR;
    propVar[0].pwszVal =
        const_cast<LPWSTR>(label.c_str());

    propId[1] = PROPID_M_BODY;
    propVar[1].vt = VT_LPWSTR;
    propVar[1].vt = VT_VECTOR | VT_UI1;
    propVar[1].caub.pElems = reinterpret_cast<UCHAR*>(utf8Text.data());
    propVar[1].caub.cElems = static_cast<ULONG>(utf8Text.size());

    msgProps.cProp = 2;
    msgProps.aPropID = propId;
    msgProps.aPropVar = propVar;
    msgProps.aStatus = status;

    HRESULT hr = MQSendMessage(
        queueHandle,
        &msgProps,
        MQ_NO_TRANSACTION);

    if (FAILED(hr))
    {
        WriteLog(L"MQSendMessage fehlgeschlagen.");
    }
    else
    {
        WriteLog(L"Nachricht geschrieben: " + label);
    }
}

// ------------------------------------------------------------

std::wstring ExtractTagValue(const std::wstring& xml, const std::wstring& tagName)
{
    std::wstring startTag =
        L"<" + tagName + L">";

    std::wstring endTag =
        L"</" + tagName + L">";

    size_t start =
        xml.find(startTag);

    if (start == std::wstring::npos)
    {
        return L"";
    }

    start += startTag.length();

    size_t end =
        xml.find(endTag, start);

    if (end == std::wstring::npos)
    {
        return L"";
    }

    std::wstring value =
        xml.substr(start, end - start);

    while (!value.empty() && iswspace(value.front()))
    {
        value.erase(value.begin());
    }

    while (!value.empty() && iswspace(value.back()))
    {
        value.pop_back();
    }

    return value;
}

// ------------------------------------------------------------

std::wstring BuildStoredProcedureName(const std::wstring& commandId, const std::wstring& storedProcedureId)
{
    // Beispiel:
    // CommandId = 9
    // StoredProcedureId = 00001
    // Ergebnis = [dbo].[SP90001]

    if (commandId.empty() || storedProcedureId.empty())
    {
        return L"[dbo].[SP00000]";
    }

    if (storedProcedureId.length() == 5)
    {
        return L"[dbo].[SP" + commandId + storedProcedureId.substr(1) + L"]";
    }

    return L"[dbo].[SP" + commandId + storedProcedureId + L"]";
}

// ------------------------------------------------------------

std::string WStringToUtf8(const std::wstring& text)
{
    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);

    if (sizeNeeded <= 0)
    {
        return "";
    }

    std::string result(sizeNeeded - 1, '\0');

    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.c_str(),
        -1,
        result.data(),
        sizeNeeded,
        nullptr,
        nullptr);

    return result;
}

// ------------------------------------------------------------

std::wstring Utf8ToWString(const std::string& text)
{
    int sizeNeeded = MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        nullptr,
        0);

    if (sizeNeeded <= 0)
    {
        return L"";
    }

    std::wstring result(sizeNeeded, L'\0');

    MultiByteToWideChar(
        CP_UTF8,
        0,
        text.c_str(),
        static_cast<int>(text.size()),
        result.data(),
        sizeNeeded);

    return result;
}

// std::wstring ExecuteStoredProcedureXml(const std::wstring& procedureName)
std::wstring ExecuteStoredProcedureXml(const std::wstring& procedureName, const std::vector<DbParameter>& parameters)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    bool comInitialized = SUCCEEDED(hr);

    IDBInitialize* pIDBInitialize = nullptr;

    hr = CoCreateInstance(
        CLSID_SQLOLEDB,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IDBInitialize,
        reinterpret_cast<void**>(&pIDBInitialize));

    if (FAILED(hr))
    {
        WriteLog(L"CoCreateInstance SQLOLEDB fehlgeschlagen.");
        return L"<Error>OLEDB Provider konnte nicht erstellt werden.</Error>";
    }

    IDBProperties* pIDBProperties = nullptr;

    hr = pIDBInitialize->QueryInterface(
        IID_IDBProperties,
        reinterpret_cast<void**>(&pIDBProperties));

    if (FAILED(hr))
    {
        pIDBInitialize->Release();
        return L"<Error>IDBProperties Fehler.</Error>";
    }

    DBPROP dbProps[6]{};

    dbProps[0].dwPropertyID = DBPROP_INIT_DATASOURCE;
    dbProps[0].dwOptions = DBPROPOPTIONS_REQUIRED;
    dbProps[0].colid = DB_NULLID;
    dbProps[0].vValue.vt = VT_BSTR;
    dbProps[0].vValue.bstrVal = SysAllocString(L"ASDE\\MSSQLSERVER002");

    dbProps[1].dwPropertyID = DBPROP_INIT_CATALOG;
    dbProps[1].dwOptions = DBPROPOPTIONS_REQUIRED;
    dbProps[1].colid = DB_NULLID;
    dbProps[1].vValue.vt = VT_BSTR;
    dbProps[1].vValue.bstrVal = SysAllocString(L"AdventureWorks2019");

    dbProps[2].dwPropertyID = DBPROP_AUTH_USERID;
    dbProps[2].dwOptions = DBPROPOPTIONS_REQUIRED;
    dbProps[2].colid = DB_NULLID;
    dbProps[2].vValue.vt = VT_BSTR;
    dbProps[2].vValue.bstrVal = SysAllocString(L"sa");

    dbProps[3].dwPropertyID = DBPROP_AUTH_PASSWORD;
    dbProps[3].dwOptions = DBPROPOPTIONS_REQUIRED;
    dbProps[3].colid = DB_NULLID;
    dbProps[3].vValue.vt = VT_BSTR;
    dbProps[3].vValue.bstrVal = SysAllocString(L"TWtw13102050");

    dbProps[4].dwPropertyID = DBPROP_INIT_PROVIDERSTRING;
    dbProps[4].dwOptions = DBPROPOPTIONS_REQUIRED;
    dbProps[4].colid = DB_NULLID;
    dbProps[4].vValue.vt = VT_BSTR;
    dbProps[4].vValue.bstrVal =
        SysAllocString(L"Persist Security Info=False;Pooling=False;Encrypt=False;TrustServerCertificate=False;");

    dbProps[5].dwPropertyID = DBPROP_INIT_TIMEOUT;
    dbProps[5].dwOptions = DBPROPOPTIONS_REQUIRED;
    dbProps[5].colid = DB_NULLID;
    dbProps[5].vValue.vt = VT_I4;
    dbProps[5].vValue.lVal = 15;

    DBPROPSET propSet{};
    propSet.guidPropertySet = DBPROPSET_DBINIT;
    propSet.cProperties = 6;
    propSet.rgProperties = dbProps;
    
    hr = pIDBProperties->SetProperties(1, &propSet);

    for (int i = 0; i < 6; i++)
    {
        VariantClear(&dbProps[i].vValue);
    }

    pIDBProperties->Release();

    if (FAILED(hr))
    {
        pIDBInitialize->Release();
        return L"<Error>SetProperties fehlgeschlagen.</Error>";
    }

    hr = pIDBInitialize->Initialize();

    if (FAILED(hr))
    {
        pIDBInitialize->Release();
        WriteLog(L"SQL Verbindung fehlgeschlagen.");
        return L"<Error>SQL Verbindung fehlgeschlagen.</Error>";
    }

    IDBCreateSession* pCreateSession = nullptr;

    hr = pIDBInitialize->QueryInterface(
        IID_IDBCreateSession,
        reinterpret_cast<void**>(&pCreateSession));

    if (FAILED(hr))
    {
        pIDBInitialize->Uninitialize();
        pIDBInitialize->Release();
        return L"<Error>IDBCreateSession Fehler.</Error>";
    }

    IDBCreateCommand* pCreateCommand = nullptr;

    hr = pCreateSession->CreateSession(
        nullptr,
        IID_IDBCreateCommand,
        reinterpret_cast<IUnknown**>(&pCreateCommand));

    pCreateSession->Release();

    if (FAILED(hr))
    {
        pIDBInitialize->Uninitialize();
        pIDBInitialize->Release();
        return L"<Error>IDBCreateCommand Fehler.</Error>";
    }

    ICommandText* pCommandText = nullptr;

    hr = pCreateCommand->CreateCommand(
        nullptr,
        IID_ICommandText,
        reinterpret_cast<IUnknown**>(&pCommandText));

    pCreateCommand->Release();

    if (FAILED(hr))
    {
        pIDBInitialize->Uninitialize();
        pIDBInitialize->Release();
        return L"<Error>ICommandText Fehler.</Error>";
    }

    // std::wstring sql = L"EXEC [dbo].[SP90001]";//      EXEC //dbo." + procedureName;

    std::wstring sql = BuildSqlCommand(procedureName, parameters);

    WriteLog(L"SQL Befehl: " + sql);

    hr = pCommandText->SetCommandText(
        DBGUID_DBSQL,
        sql.c_str());

    if (FAILED(hr))
    {
        pCommandText->Release();
        pIDBInitialize->Uninitialize();
        pIDBInitialize->Release();
        return L"<Error>SetCommandText Fehler.</Error>";
    }

    IRowset* pRowset = nullptr;

    hr = pCommandText->Execute(
        nullptr,
        IID_IRowset,
        nullptr,
        nullptr,
        reinterpret_cast<IUnknown**>(&pRowset));

    pCommandText->Release();

    if (FAILED(hr))
    {
        pIDBInitialize->Uninitialize();
        pIDBInitialize->Release();
        WriteLog(L"Execute Stored Procedure fehlgeschlagen: " + sql);
        return L"<Error>Stored Procedure konnte nicht ausgeführt werden.</Error>";
    }

    IAccessor* pAccessor = nullptr;

    hr = pRowset->QueryInterface(
        IID_IAccessor,
        reinterpret_cast<void**>(&pAccessor));

    if (FAILED(hr))
    {
        pRowset->Release();
        pIDBInitialize->Uninitialize();
        pIDBInitialize->Release();
        return L"<Error>IAccessor Fehler.</Error>";
    }

    struct RowData
    {
        DBSTATUS status;
        DBLENGTH length;
        wchar_t xml[2 * 1024 * 1024];
    };

    DBBINDING binding{};
    binding.iOrdinal = 1;
    binding.obStatus = offsetof(RowData, status);
    binding.obLength = offsetof(RowData, length);
    binding.obValue = offsetof(RowData, xml);
    binding.dwPart = DBPART_VALUE | DBPART_STATUS | DBPART_LENGTH;
    binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
    binding.eParamIO = DBPARAMIO_NOTPARAM;
    binding.cbMaxLen = sizeof(((RowData*)0)->xml);
    binding.wType = DBTYPE_WSTR;

    HACCESSOR hAccessor = NULL;

    hr = pAccessor->CreateAccessor(
        DBACCESSOR_ROWDATA,
        1,
        &binding,
        sizeof(RowData),
        &hAccessor,
        nullptr);

    if (FAILED(hr))
    {
        pAccessor->Release();
        pRowset->Release();
        pIDBInitialize->Uninitialize();
        pIDBInitialize->Release();
        return L"<Error>CreateAccessor Fehler.</Error>";
    }

    HROW hRow = NULL;
    HROW* pRows = &hRow;
    DBCOUNTITEM rowsObtained = 0;

    hr = pRowset->GetNextRows(
        DB_NULL_HCHAPTER,
        0,
        1,
        &rowsObtained,
        &pRows);

    std::wstring resultXml;

    if (SUCCEEDED(hr) && rowsObtained == 1)
    {
        auto data = std::make_unique<RowData>();

        hr = pRowset->GetData(hRow, hAccessor, data.get());

        if (SUCCEEDED(hr) && data->status == DBSTATUS_S_OK)
        {
            size_t charCount = data->length / sizeof(wchar_t);

            resultXml.assign(data->xml, charCount);

            while (!resultXml.empty() && resultXml.back() == L'\0')
            {
                resultXml.pop_back();
            }

            WriteLog(L"SQL XML Länge: " + std::to_wstring(resultXml.length()));
        }
        else
        {
            resultXml = L"<Error>Keine XML-Daten gelesen.</Error>";
        }

        pRowset->ReleaseRows(
            1,
            &hRow,
            nullptr,
            nullptr,
            nullptr);
    }
    else
    {
        resultXml = L"<Error>Keine Zeile von Stored Procedure erhalten.</Error>";
    }

    pAccessor->ReleaseAccessor(hAccessor, nullptr);
    pAccessor->Release();
    pRowset->Release();

    pIDBInitialize->Uninitialize();
    pIDBInitialize->Release();

    if (comInitialized)
    {
        CoUninitialize();
    }

    WriteLog(L"Stored Procedure ausgeführt: " + procedureName);

    return resultXml;
}

std::vector<DbParameter> ExtractParameters(const std::wstring& xml)
{
    std::vector<DbParameter> result;

    size_t pos = 0;

    while (true)
    {
        size_t start = xml.find(L"<Parameter>", pos);

        if (start == std::wstring::npos)
        {
            break;
        }

        size_t end = xml.find(L"</Parameter>", start);

        if (end == std::wstring::npos)
        {
            break;
        }

        std::wstring block =
            xml.substr(start, end - start);

        std::wstring value =
            ExtractTagValue(block, L"ParameterValue");

        if (!value.empty())
        {
            int number =
                static_cast<int>(result.size()) + 1;

            wchar_t nameBuffer[32]{};

            swprintf_s(
                nameBuffer,
                L"Parameter%03d",
                number);

            DbParameter parameter;
            parameter.Name = nameBuffer;
            parameter.Value = value;

            result.push_back(parameter);
        }

        pos = end + wcslen(L"</Parameter>");
    }

    WriteLog(
        L"Parameter gefunden: " +
        std::to_wstring(result.size()));

    return result;
}

std::wstring EscapeSql(const std::wstring& value)
{
    std::wstring result;

    for (wchar_t ch : value)
    {
        if (ch == L'\'')
        {
            result += L"''";
        }
        else
        {
            result += ch;
        }
    }

    return result;
}

std::wstring BuildSqlCommand(
    const std::wstring& procedureName,
    const std::vector<DbParameter>& parameters)
{
    std::wstring sql =
        L"EXEC " + procedureName;

    for (size_t i = 0; i < parameters.size(); i++)
    {
        if (i == 0)
        {
            sql += L" ";
        }
        else
        {
            sql += L", ";
        }

        sql += L"@" + parameters[i].Name;
        sql += L" = N'";
        sql += EscapeSql(parameters[i].Value);
        sql += L"'";
    }

    return sql;
}