-- Wavestorm SDK, high-level Lua layer (Defold and plain Lua 5.1+/LuaJIT). `local ws = require "wavestorm.ws"`.
-- Wraps the native `wavestorm` table with the pieces a game needs: launcher-required startup, lobbies with ready flags,
-- P2P networking between lobby members, structured presence and cloud-save slots. Everything async is cb(ok, result).
-- Without the launcher ws.available stays false and every call fails with "unavailable", so a game can still run offline.
local native = rawget(_G, "wavestorm") or require("wavestorm")
local M = { native = native, available = false, error = nil }

local MESSAGES = {
  no_runtime = "Online play is not available: start the game from the Wavestorm launcher.",
  connect = "The Wavestorm launcher is not running.",
  auth = "The Wavestorm launcher rejected this game. Restart it from the launcher.",
  timeout = "The Wavestorm launcher did not answer.",
  protocol = "The Wavestorm launcher and this game are incompatible. Update the launcher.",
}
function M.error_message(code) return MESSAGES[code] or ("Wavestorm error: " .. tostring(code)) end

local function call(method, params, cb)
  cb = cb or function() end
  if not M.available then return cb(false, { error = "unavailable" }) end
  if not native.has_capability("lobby") and method:sub(1, 6) == "lobby." then return cb(false, { error = "unsupported" }) end
  native.call(method, params or {}, cb)
end
M.call = call

-- Steam-style: call first thing in init(). Returns true when the game must quit because the launcher is starting it properly.
function M.restart_if_needed(slug) return native.restart_if_needed(slug) == 1 end

-- ok, err = ws.init(). err is a short code; ws.error_message(err) is a sentence for the player.
function M.init()
  local ok, err = native.init()
  M.available, M.error = ok and true or false, err
  if ok then
    native.net_listen(function() end)
    native.on("join", function(info) M._join(info) end)
    for _, ev in ipairs({ "member_joined", "member_left", "member_kicked", "member_updated", "updated", "started", "chat", "kicked" }) do
      native.on("lobby." .. ev, function(d) M.lobby._event(ev, d) end)
    end
    native.on("net.packet", function(user, reliable, bytes) if M.net._handler then M.net._handler(user, bytes, reliable) end end)
    native.on("net.connected", function(d) M.net._peers[d.userId] = true end)
    native.on("net.disconnected", function(d) M.net._peers[d.userId] = nil end)
    M._join(native.join_info())
  end
  return ok, err
end
function M.shutdown() M.available = false; native.shutdown() end
function M.update() native.run_callbacks() end -- plain Lua only; the Defold extension pumps this every frame

function M.user() return M.available and native.user() or nil end
function M.friends(cb) call("friends.list", nil, cb) end
function M.invite(user_id, cb)
  if M.lobby.current then return M.lobby.invite(user_id, cb) end
  call("invites.send", { userId = user_id }, cb)
end
function M.unlock_achievement(name, cb) call("achievements.unlock", { apiName = name }, cb) end
function M.achievements(cb) call("achievements.list", nil, cb) end
function M.on(event, fn) native.on(event, fn) end
-- Presence: text plus optional key/value table (values are strings), shown to friends and in the overlay.
function M.set_presence(text, kv) call("presence.set", { text = text, kv = kv }, nil) end

-- ---- lobbies
local L = { current = nil, _handlers = {} }
M.lobby = L
local function emit(name, ...) for _, fn in ipairs(L._handlers[name] or {}) do fn(...) end end
-- Events: "joined", "left", "updated", "member_joined", "member_left", "member_updated", "started", "chat", "kicked".
function L.on(name, fn) local h = L._handlers; h[name] = h[name] or {}; table.insert(h[name], fn) end

local function adopt(lobby)
  local had = L.current
  L.current = lobby
  if lobby then M.net._connect_members(lobby) end
  if lobby and not had then emit("joined", lobby) end
  if had and not lobby then emit("left") end
end
local function keep(cb) return function(ok, r) if ok then adopt(r) end; if cb then cb(ok, r) end end end

function L._event(name, d)
  if name == "kicked" then adopt(nil); return emit("kicked") end
  local function done(lobby) emit(name, d, lobby) end
  if d.lobby then adopt(d.lobby); return done(d.lobby) end
  if name == "chat" then return emit("chat", d.from, d.text) end
  -- joins/leaves carry no lobby snapshot: refetch it
  call("lobby.get", { lobbyId = d.id }, function(ok, r) if ok and r.members then adopt(r); done(r) end end)
end

-- create{type="friends"|"public"|"invite", max=8, min=2, data={mode="ffa"}}
function L.create(o, cb) o = o or {}; call("lobby.create", { type = o.type, maxMembers = o.max, minMembers = o.min, data = o.data }, keep(cb)) end
function L.join(id, cb) call("lobby.join", { lobbyId = id }, keep(cb)) end
function L.list(filter, cb) call("lobby.list", { filter = filter }, cb) end
function L.leave(cb)
  if not L.current then return (cb or function() end)(true, {}) end
  local id = L.current.id
  call("lobby.leave", { lobbyId = id }, function(ok, r) if ok then adopt(nil) end; if cb then cb(ok, r) end end)
end
local function need(cb) if L.current then return true end; (cb or function() end)(false, { error = "not_in_lobby" }); return false end
function L.set_ready(ready, cb) if need(cb) then call("lobby.setMember", { lobbyId = L.current.id, ready = ready }, keep(cb)) end end
function L.set_member_data(data, cb) if need(cb) then call("lobby.setMember", { lobbyId = L.current.id, data = data }, keep(cb)) end end
function L.set_data(data, cb) if need(cb) then call("lobby.update", { lobbyId = L.current.id, data = data }, keep(cb)) end end
function L.set_joinable(on, cb) if need(cb) then call("lobby.update", { lobbyId = L.current.id, joinable = on }, keep(cb)) end end
function L.start(cb) if need(cb) then call("lobby.start", { lobbyId = L.current.id }, keep(cb)) end end -- host; fails "not_ready"/"not_enough_players"
function L.invite(user_id, cb) if need(cb) then call("lobby.invite", { lobbyId = L.current.id, userId = user_id }, cb) end end
function L.kick(user_id, cb) if need(cb) then call("lobby.kick", { lobbyId = L.current.id, userId = user_id }, cb) end end
function L.set_host(user_id, cb) if need(cb) then call("lobby.setHost", { lobbyId = L.current.id, userId = user_id }, cb) end end
function L.chat(text, cb) if need(cb) then call("lobby.chat", { lobbyId = L.current.id, text = text }, cb) end end
function L.is_host() local u = M.user(); return L.current ~= nil and u ~= nil and L.current.hostId == u.id end
function L.all_ready() if not L.current then return false end; for _, m in ipairs(L.current.members) do if not m.ready then return false end end; return true end
-- Members sorted by user id: every player computes the same order, so teams (e.g. odd/even index) agree without talking.
function L.members_sorted()
  local out = {}
  for _, m in ipairs(L.current and L.current.members or {}) do out[#out + 1] = m end
  table.sort(out, function(a, b) return a.id < b.id end)
  return out
end

-- A friend's invite or "join" arrives as "lobby:<id>": join it. Anything else is left to the game via ws.on_join.
function M._join(info)
  if type(info) ~= "string" or info == "" then return end
  local id = info:match("^lobby:(%d+)$")
  if id then L.join(tonumber(id), function(ok, r) if M.on_join then M.on_join(info, ok, r) end end)
  elseif M.on_join then M.on_join(info, true, nil) end
end

-- ---- P2P between lobby members. The lower user id dials the higher one; everybody listens. Packets are raw strings (<= 16000 bytes).
local N = { _peers = {}, _handler = nil, _dialing = {} }
M.net = N
function N._connect_members(lobby)
  local me = M.user(); if not me then return end
  for _, m in ipairs(lobby.members) do
    if m.id > me.id and not N._peers[m.id] and not N._dialing[m.id] then
      N._dialing[m.id] = true
      native.net_connect(m.id, function() N._dialing[m.id] = nil end)
    end
  end
end
function N.send(user_id, bytes, reliable) return native.net_send(user_id, bytes, reliable ~= false) end
-- To everyone in the lobby except yourself (the host-authoritative pattern: clients call N.send(host_id, ...)).
function N.broadcast(bytes, reliable)
  local me, n = M.user(), 0
  for _, m in ipairs(L.current and L.current.members or {}) do
    if me and m.id ~= me.id and N._peers[m.id] and N.send(m.id, bytes, reliable) then n = n + 1 end
  end
  return n
end
function N.on_packet(fn) N._handler = fn end -- fn(user_id, bytes, reliable)
function N.connected(user_id) return N._peers[user_id] == true end

-- ---- cloud-save slots: opaque strings, up to ~380 KB each (the launcher limits one call to 512 KB of base64).
local S = {}
M.saves = S
local B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local function b64(s)
  local out = {}
  for i = 1, #s, 3 do
    local a, b, c = s:byte(i, i + 2)
    local n = a * 65536 + (b or 0) * 256 + (c or 0)
    local c1, c2, c3, c4 = math.floor(n / 262144) % 64, math.floor(n / 4096) % 64, math.floor(n / 64) % 64, n % 64
    out[#out + 1] = B:sub(c1 + 1, c1 + 1) .. B:sub(c2 + 1, c2 + 1) .. (b and B:sub(c3 + 1, c3 + 1) or "=") .. (c and B:sub(c4 + 1, c4 + 1) or "=")
  end
  return table.concat(out)
end
local function unb64(s)
  local out, rev = {}, {}
  for i = 1, #B do rev[B:byte(i)] = i - 1 end
  s = s:gsub("[^%w%+/=]", "")
  for i = 1, #s, 4 do
    local a, b, c, d = s:byte(i, i + 3)
    local n = rev[a] * 262144 + rev[b] * 4096 + (rev[c] or 0) * 64 + (rev[d] or 0)
    local x, y, z = math.floor(n / 65536) % 256, math.floor(n / 256) % 256, n % 256
    out[#out + 1] = string.char(x) .. (c ~= 61 and string.char(y) or "") .. (d ~= 61 and string.char(z) or "")
  end
  return table.concat(out)
end
M._b64, M._unb64 = b64, unb64
function S.list(cb) call("saves.list", nil, cb) end
-- write(slot, data, cb[, force]): fails with error "conflict" (result.current) if another computer saved meanwhile; retry with force=true to overwrite.
function S.write(slot, data, cb, force)
  call("saves.write", { slot = slot, data = b64(data), baseRevision = S._rev and S._rev[slot] or 0, force = force }, function(ok, r)
    if ok then S._rev = S._rev or {}; S._rev[slot] = r.revision end
    if cb then cb(ok, r) end
  end)
end
function S.read(slot, cb)
  call("saves.read", { slot = slot }, function(ok, r)
    if not ok then return cb(false, r) end
    S._rev = S._rev or {}; S._rev[slot] = r.revision
    cb(true, unb64(r.data))
  end)
end
function S.delete(slot, cb) call("saves.delete", { slot = slot }, cb) end

return M
