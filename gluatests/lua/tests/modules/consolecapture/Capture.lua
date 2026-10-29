-- Run in Linux x86-64 SRCDS with -holylib_enable_consolecapture 1.
local enabled = SERVER and consolecapture ~= nil
local function joined(batch)
    local text = {}
    for _, record in ipairs(batch.records) do text[#text + 1] = record.text end
    return table.concat(text)
end
return {
    groupName = "consolecapture native listener",
    cases = {
        {
            name = "Captures print, Msg, MsgC and warnings without condebug",
            when = enabled,
            func = function()
                local cursor = consolecapture.GetCursor()
                print("holy_capture_print")
                Msg("holy_capture_msg\n")
                MsgC(Color(123, 45, 67), "holy_capture_color\n")
                ErrorNoHalt("holy_capture_warning\n")
                local text = joined(consolecapture.Read(cursor, 256, 65536))
                for _, expected in ipairs({ "holy_capture_print", "holy_capture_msg", "holy_capture_color", "holy_capture_warning" }) do
                    assert(text:find(expected, 1, true), "Logging listener missed " .. expected)
                end
            end
        },
        {
            name = "Byte-fragment cursors reconstruct output exactly",
            when = enabled,
            func = function()
                local cursor = consolecapture.GetCursor()
                Msg("holy_fragment_test\n")
                local full = consolecapture.Read(cursor, 256, 65536)
                local text = {}
                repeat
                    local batch = consolecapture.Read(cursor, 1, 1)
                    text[#text + 1] = joined(batch)
                    assert(not batch.reset)
                    cursor = batch.cursor
                until cursor == full.cursor
                assert(table.concat(text) == joined(full))
            end
        },
        {
            name = "Observer feedback is displayed but not captured; unsubscribe releases reference",
            when = enabled,
            async = true,
            timeout = 3,
            func = function()
                local cursor = consolecapture.GetCursor()
                local id
                id = consolecapture.Subscribe(function(batch)
                    if not joined(batch):find("holy_observer_trigger", 1, true) then return end
                    local before = consolecapture.GetCursor()
                    print("holy_observer_feedback")
                    assert(consolecapture.GetCursor() == before)
                    assert(consolecapture.Unsubscribe(id))
                    assert(not consolecapture.Unsubscribe(id))
                    done()
                end, cursor)
                Msg("holy_observer_trigger\n")
            end
        },
        {
            name = "Expired file-era cursors reset explicitly",
            when = enabled,
            func = function()
                local batch = consolecapture.Read("file-era:1:2", 1, 1)
                assert(batch.reset and batch.from_cursor and batch.cursor)
                local stats = consolecapture.GetStats()
                assert(stats.max_bytes == 4194304 and stats.max_records == 4096)
                assert(stats.earliest_cursor and stats.cursor and stats.dropped_bytes >= 0)
            end
        },
        {
            name = "Module disable removes the listener and Lua references; re-enable registers once",
            when = enabled,
            func = function()
                local old = consolecapture.GetCursor()
                local id = consolecapture.Subscribe(function() error("Old subscription survived disable") end)
                local convar = assert(GetConVar("holylib_enable_consolecapture"))
                convar:SetBool(false)
                assert(consolecapture == nil)
                Msg("holy_disabled_listener_marker\n")
                convar:SetBool(true)
                assert(consolecapture and consolecapture.Read(old, 256, 65536).reset)
                assert(not consolecapture.Unsubscribe(id))
                local cursor = consolecapture.GetCursor()
                Msg("holy_reenabled_listener_marker\n")
                local text = joined(consolecapture.Read(cursor, 256, 65536))
                local _, count = text:gsub("holy_reenabled_listener_marker", "")
                assert(count == 1, "Expected one registered listener")
                assert(not text:find("holy_disabled_listener_marker", 1, true))
            end
        }
    }
}
