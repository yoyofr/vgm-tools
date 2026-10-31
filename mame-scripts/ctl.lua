local mach = manager.machine
local Z = os.getenv("ZDIR")
local cpu = mach.devices[":maincpu"]
local mem = cpu.spaces["program"]
local f = io.open(Z.."/tap_ctl.log", "w")
local function now() return mach.time:as_double() end
tap1 = mem:install_write_tap(0x1fbe0000, 0x1fbe01ff, "mbx", function(offset, data, mask)
  if data ~= 0 then f:write(string.format("%9.3f MBX %08x %08x %08x\n", now(), offset, data, mask)); f:flush() end
end)
tap2 = mem:install_write_tap(0x1fba0000, 0x1fba0003, "irq", function(offset, data, mask)
  f:write(string.format("%9.3f IRQ\n", now())); f:flush()
end)
-- list ports/fields
local pf = io.open(Z.."/ports.txt", "w")
fields = {}
for pname, port in pairs(mach.ioport.ports) do
  for fname, field in pairs(port.fields) do
    pf:write(pname .. " | " .. fname .. "\n")
    fields[pname .. "|" .. fname] = field
  end
end
pf:close()
local log = io.open(Z.."/ctl_out.log", "w")
local function L(s) log:write(string.format("%9.3f %s\n", now(), s)); log:flush() end
L("started")

if os.getenv("DIAG") == "5" then
  local snd = mach.devices[":taito_zoom:mn10200"]
  local sp = snd.spaces["program"]
  local rf = io.open(Z.."/songreads.log", "w")
  local nlog = 0
  songtap = sp:install_read_tap(tonumber(os.getenv("TAPLO") or "a2f24", 16), tonumber(os.getenv("TAPHI") or "a4525", 16), "song", function(offset, data, mask)
    nlog = nlog + 1
    if nlog <= 2000000 then rf:write(string.format("%.4f %06x %04x %04x\n", mach.time:as_double(), offset, data, mask)) end
    if nlog % 500 == 0 then rf:flush() end
  end)
  L("song read tap installed")
elseif os.getenv("DIAG") then
  local snd = mach.devices[":taito_zoom:mn10200"]
  -- allocation failure path (note dropped): log and continue
  snd.debug:bpset(0x821b9, "1", 'logerror "ALLOCFAIL\\n"; g')
  if os.getenv("DIAG") == "3" then
    snd.debug:bpset(0x853a2, "1", 'logerror "EV %X trk=%06X pos=%06X st=%02X d1=%02X d2=%02X ch=%02X\\n",totalcycles,(d@(a3+2a)&ffffff),(d@((d@(a3+2a)&ffffff)+4)&ffffff),b@((d@(a3+2a)&ffffff)+c),b@((d@(a3+2a)&ffffff)+d),b@((d@(a3+2a)&ffffff)+e),b@((d@(a3+2a)&ffffff)+b); g')
  end
  if os.getenv("DIAG") == "4" then
    snd.debug:wpset(snd.spaces["program"], "r", 0xa2f24, 0x1600, "1", 'logerror "RD %X pc=%06X\\n",wpaddr,pc; g')
  end
  if os.getenv("DIAG") == "2" then snd.debug:bpset(0x82197, "1", 'logerror "NOTEON\\n"; g') end
  L("diag bp set")
end
local frame = 0
local pending = {}
emu.register_frame_done(function()
  frame = frame + 1
  for k, v in pairs(pending) do if frame >= v.at then v.fld:clear_value(); pending[k] = nil; L("release "..k) end end
  local cf = io.open(Z.."/cmd.txt", "r")
  if cf then
    local lines = {}
    for line in cf:lines() do table.insert(lines, line) end
    cf:close(); os.remove(Z.."/cmd.txt")
    L("got "..#lines.." line(s): "..table.concat(lines, " ; "))
    for _, line in ipairs(lines) do
     local ok, err = pcall(function()
      local op, a, b = line:match("^(%S+)%s*(%S*)%s*(%S*)")
      if op == "set" then
        local fld = fields[a:gsub("_", " ")]
        if fld then fld:set_value(tonumber(b)); L("set "..a.." "..b) else L("no field "..a) end
      elseif op == "clear" then
        local fld = fields[a:gsub("_", " ")]
        if fld then fld:clear_value(); L("clear "..a) else L("no field "..a) end
      elseif op == "snap" then
        mach.video:snapshot(); L("snap")
      elseif op == "tap" then
        local fld = fields[a:gsub("_", " ")]
        if fld then fld:set_value(1); pending[a] = {fld = fld, at = frame + 2}; L("tap "..a) else L("no field "..a) end
      elseif op == "peek" then
        local sp = mach.devices[":taito_zoom:mn10200"].spaces["program"]
        local addr = tonumber(a, 16); local t = {}
        for i = 0, 31 do t[#t+1] = string.format("%02x", sp:read_u8(addr + i)) end
        L(string.format("peek %06x: %s", addr, table.concat(t, " ")))
      elseif op == "dump" then
        local sp = mach.devices[":taito_zoom:mn10200"].spaces["program"]
        local addr = tonumber(a, 16); local len = tonumber(b, 16)
        local out = io.open(Z.."/dump_"..a..".bin", "wb")
        local buf = {}
        for i = 0, len - 1 do buf[#buf+1] = string.char(sp:read_u8(addr + i)) if #buf >= 4096 then out:write(table.concat(buf)); buf = {} end end
        out:write(table.concat(buf)); out:close(); L("dumped "..a.." "..b)
      elseif op == "time" then
        L("time")
      elseif op == "quit" then
        mach:exit()
      end
     end)
     if not ok then L("ERROR "..tostring(err)) end
    end
  end
end)
