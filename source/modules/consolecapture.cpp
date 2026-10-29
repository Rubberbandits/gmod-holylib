#include "LuaInterface.h"
#include "module.h"
#include "lua.h"
#include "mcp/console_buffer.h"
#include <map>
#include <memory>
#include <cmath>
#include <cstring>
#if SYSTEM_LINUX && ARCHITECTURE_X86_64
#include "tier0/logging.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

class CConsoleCaptureModule : public IModule {
public:
    void Init(CreateInterfaceFn*, CreateInterfaceFn*) override;
    void Shutdown() override;
    void LuaInit(GarrysMod::Lua::ILuaInterface*, bool) override;
    void LuaShutdown(GarrysMod::Lua::ILuaInterface*) override;
    void LuaThink(GarrysMod::Lua::ILuaInterface*) override;
    const char* Name() override { return "consolecapture"; }
    int Compatibility() override { return LINUX64; }
    bool SupportsMultipleLuaStates() override { return false; }
    bool IsEnabledByDefault() override { return false; }
    bool CanDisableAtRuntime() override { return ThreadInMainThread() && HolyConsole::deliveryDepth == 0; }
};
static CConsoleCaptureModule g_pConsoleCaptureModule;
IModule* pConsoleCaptureModule = &g_pConsoleCaptureModule;
static std::shared_ptr<HolyConsole::Buffer> g_capture;

#if SYSTEM_LINUX && ARCHITECTURE_X86_64
class ConsoleListener : public ILoggingListener {
public:
    void Log(const LoggingContext_t* context, const tchar* text) override {
        auto capture = std::atomic_load(&g_capture);
        if (!capture || HolyConsole::deliveryDepth || !context || !text) return;
        const auto* channel = LoggingSystem_GetChannel(context->m_ChannelID);
        unsigned char color[4] = { static_cast<unsigned char>(context->m_Color.r()), static_cast<unsigned char>(context->m_Color.g()),
            static_cast<unsigned char>(context->m_Color.b()), static_cast<unsigned char>(context->m_Color.a()) };
        const char* level = context->m_Severity == LS_WARNING ? "warning" : (context->m_Severity >= LS_ASSERT ? "error" : "info");
        capture->Append(text, std::strlen(text), channel ? channel->m_Name : "console", level, color, context->m_Severity, context->m_ChannelID);
    }
};
static ConsoleListener g_listener;
#endif

struct ConsoleSubscription { int reference; std::string cursor; };
class LuaConsoleCaptureModuleData : public Lua::ModuleData {
public:
    uint32_t nextID = 1;
    size_t nextDispatch = 0;
    std::map<uint32_t, ConsoleSubscription> subscriptions;
};
LUA_GetModuleData(LuaConsoleCaptureModuleData, g_pConsoleCaptureModule, ConsoleCapture)

static void PushBatch(GarrysMod::Lua::ILuaInterface* lua, const HolyConsole::Batch& batch) {
    lua->CreateTable();
    lua->PushString(batch.cursor.c_str()); lua->SetField(-2, "cursor");
    lua->PushString(batch.from.c_str()); lua->SetField(-2, "from_cursor");
    lua->PushBool(batch.reset); lua->SetField(-2, "reset");
    lua->PushNumber(static_cast<double>(batch.dropped)); lua->SetField(-2, "dropped");
    lua->PushBool(batch.hasMore); lua->SetField(-2, "has_more");
    lua->PreCreateTable(batch.records.size(), 0);
    int index = 0;
    for (const auto& record : batch.records) {
        lua->CreateTable();
        lua->PushString(record.text.data(), static_cast<unsigned int>(record.text.size())); lua->SetField(-2, "text");
        lua->PushString(std::to_string(record.sequence).c_str()); lua->SetField(-2, "sequence");
        lua->PushString(record.cursor.c_str()); lua->SetField(-2, "cursor");
        lua->PushString(record.channel.c_str()); lua->SetField(-2, "channel");
        lua->PushString(record.level.c_str()); lua->SetField(-2, "level");
        lua->PushNumber(record.severity); lua->SetField(-2, "severity");
        lua->PushNumber(record.channelID); lua->SetField(-2, "channel_id");
        lua->PushNumber(record.timestamp); lua->SetField(-2, "timestamp");
        lua->PushBool(record.truncated); lua->SetField(-2, "truncated");
        lua->CreateTable();
        const char* names[] = { "r", "g", "b", "a" };
        for (int i = 0; i < 4; ++i) { lua->PushNumber(record.color[i]); lua->SetField(-2, names[i]); }
        lua->SetField(-2, "color");
        Lua::RawSetI(lua, -2, ++index);
    }
    lua->SetField(-2, "records");
}
static std::shared_ptr<HolyConsole::Buffer> Capture(GarrysMod::Lua::ILuaInterface* lua) {
    auto capture = std::atomic_load(&g_capture);
    if (!capture) lua->ThrowError("Native console capture is unavailable; enable consolecapture on Linux x86-64.");
    return capture;
}
LUA_FUNCTION_STATIC(Console_GetCursor) {
    LUA->PushString(Capture(LUA)->GetCursor().c_str()); return 1;
}
LUA_FUNCTION_STATIC(Console_Read) {
    const std::string cursor = LUA->CheckStringOpt(1, "");
    const auto count = LUA->CheckNumberOpt(2, 256), bytes = LUA->CheckNumberOpt(3, 65536);
    if (!std::isfinite(count) || !std::isfinite(bytes) || count < 1 || count > 4096 || count != std::floor(count) || bytes < 1 || bytes > 1048576 || bytes != std::floor(bytes))
        LUA->ThrowError("Read limits must be integer record counts (1..4096) and bytes (1..1048576).");
    PushBatch(LUA, Capture(LUA)->Read(cursor, static_cast<size_t>(count), static_cast<size_t>(bytes))); return 1;
}
LUA_FUNCTION_STATIC(Console_Subscribe) {
    LUA->CheckType(1, GarrysMod::Lua::Type::Function);
    auto* data = GetConsoleCaptureLuaData(LUA);
    if (data->subscriptions.size() >= 128) LUA->ThrowError("Too many console subscriptions.");
    const std::string cursor = LUA->CheckStringOpt(2, Capture(LUA)->GetCursor().c_str());
    LUA->Push(1);
    const int ref = Util::ReferenceCreate(LUA, "ConsoleCapture subscription");
    const auto id = data->nextID++;
    data->subscriptions.emplace(id, ConsoleSubscription{ref, cursor});
    LUA->PushNumber(id); return 1;
}
LUA_FUNCTION_STATIC(Console_Unsubscribe) {
    auto* data = GetConsoleCaptureLuaData(LUA);
    const double id = LUA->CheckNumber(1);
    if (!std::isfinite(id) || id < 1 || id > UINT32_MAX || id != std::floor(id)) LUA->ArgError(1, "Invalid subscription id.");
    auto found = data->subscriptions.find(static_cast<uint32_t>(id));
    if (found == data->subscriptions.end()) { LUA->PushBool(false); return 1; }
    Util::ReferenceFree(LUA, found->second.reference, "ConsoleCapture unsubscribe");
    data->subscriptions.erase(found); LUA->PushBool(true); return 1;
}
LUA_FUNCTION_STATIC(Console_GetStats) {
    const auto stats = Capture(LUA)->GetStats();
    LUA->CreateTable();
    LUA->PushNumber(stats.bytes); LUA->SetField(-2, "bytes");
    LUA->PushNumber(stats.records); LUA->SetField(-2, "records");
    LUA->PushNumber(stats.maxBytes); LUA->SetField(-2, "max_bytes");
    LUA->PushNumber(stats.maxRecords); LUA->SetField(-2, "max_records");
    LUA->PushNumber(static_cast<double>(stats.dropped)); LUA->SetField(-2, "dropped");
    LUA->PushNumber(static_cast<double>(stats.droppedBytes)); LUA->SetField(-2, "dropped_bytes");
    LUA->PushString(stats.earliest.c_str()); LUA->SetField(-2, "earliest_cursor");
    LUA->PushString(stats.cursor.c_str()); LUA->SetField(-2, "cursor"); return 1;
}
void CConsoleCaptureModule::Init(CreateInterfaceFn*, CreateInterfaceFn*) {
    std::atomic_store(&g_capture, std::make_shared<HolyConsole::Buffer>());
#if SYSTEM_LINUX && ARCHITECTURE_X86_64
    LoggingSystem_RegisterLoggingListener(&g_listener);
#endif
}
void CConsoleCaptureModule::Shutdown() {
#if SYSTEM_LINUX && ARCHITECTURE_X86_64
    LoggingSystem_UnregisterLoggingListener(&g_listener);
#endif
    std::atomic_store(&g_capture, std::shared_ptr<HolyConsole::Buffer>{});
}
void CConsoleCaptureModule::LuaInit(GarrysMod::Lua::ILuaInterface* lua, bool serverInit) {
    if (serverInit || lua != g_Lua || !ThreadInMainThread()) return;
    Lua::GetLuaData(lua)->SetModuleData(m_pID, new LuaConsoleCaptureModuleData);
    Util::StartTable(lua);
    Util::AddFunc(lua, Console_GetCursor, "GetCursor"); Util::AddFunc(lua, Console_Read, "Read");
    Util::AddFunc(lua, Console_Subscribe, "Subscribe"); Util::AddFunc(lua, Console_Unsubscribe, "Unsubscribe");
    Util::AddFunc(lua, Console_GetStats, "GetStats");
    Util::FinishTable(lua, "consolecapture");
}
void CConsoleCaptureModule::LuaShutdown(GarrysMod::Lua::ILuaInterface* lua) {
    auto* data = GetConsoleCaptureLuaData(lua);
    if (data) { for (const auto& pair : data->subscriptions) Util::ReferenceFree(lua, pair.second.reference, "ConsoleCapture shutdown"); data->subscriptions.clear(); }
    Util::NukeTable(lua, "consolecapture");
}
void CConsoleCaptureModule::LuaThink(GarrysMod::Lua::ILuaInterface* lua) {
    if (!ThreadInMainThread()) return;
    auto capture = std::atomic_load(&g_capture);
    auto* data = GetConsoleCaptureLuaData(lua);
    if (!capture || !data) return;
    // Snapshot IDs so callbacks may add/remove subscriptions without invalidating iteration.
    std::vector<uint32_t> ids;
    for (const auto& pair : data->subscriptions) ids.push_back(pair.first);
    size_t budget = 128 * 1024;
    const size_t start = ids.empty() ? 0 : data->nextDispatch % ids.size();
    for (size_t visited = 0; visited < ids.size(); ++visited) {
        if (budget == 0) break;
        const auto id = ids[(start + visited) % ids.size()];
        data->nextDispatch = (start + visited + 1) % ids.size();
        auto found = data->subscriptions.find(id);
        if (found == data->subscriptions.end() || budget == 0) continue;
        auto batch = capture->Read(found->second.cursor, 256, std::min(budget, size_t(65536)));
        if (batch.records.empty() && !batch.reset) continue;
        for (const auto& record : batch.records) budget -= record.text.size();
        found->second.cursor = batch.cursor;
        Lua::ReferencePush(lua, found->second.reference); PushBatch(lua, batch);
        HolyConsole::DeliveryScope guard;
        if (!lua->CallFunctionProtected(1, 0, true)) {
            found = data->subscriptions.find(id);
            if (found != data->subscriptions.end()) { Util::ReferenceFree(lua, found->second.reference, "ConsoleCapture failed callback"); data->subscriptions.erase(found); }
        }
    }
}
