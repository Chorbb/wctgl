-- tgl_logger3.lua — запись прохождения The Guardian Legend для сверки порта (Mesen 2).
--
-- Пишет КАЖДЫЙ кадр с момента запуска: кнопки, состояние процессора, RAM (2 КБ), OAM (256 Б),
-- раз в 4 кадра — память фона. Через 30 кадров после запуска сам перезагружает игру
-- и помечает первый кадр после перезагрузки: с него порт и начнёт сверку.
-- Если автоматическая перезагрузка не сработает, на экране появится подсказка
-- нажать Reset вручную.
--
-- Нужно: Settings -> "Allow access to I/O and OS functions".

local MT = emu.memType
local ET = emu.eventType
local RAM_T = MT.nesInternalRam or MT.nesDebug or MT.cpuDebug
local OAM_T = MT.nesSpriteRam or MT.spriteRam or MT.oam
local NT_T  = MT.nesNametableRam or MT.nametableRam

local status = "starting"

local folder = emu.getScriptDataFolder()
if folder == nil or folder == "" then
  emu.addEventCallback(function()
    emu.drawString(4, 4, "TGL LOGGER: I/O ACCESS DISABLED", 0xFF4040, 0xFF000000)
  end, ET.endFrame)
  return
end
if not folder:match("[/\\]$") then folder = folder .. "\\" end

local stamp = os.date("%Y%m%d_%H%M%S")
local base = folder .. "run_" .. stamp
local bin = io.open(base .. ".bin", "wb")
bin:write("TGLLOG3\n")

local function u16(n) return string.char(n & 0xFF, (n >> 8) & 0xFF) end
local function u32(n)
  return string.char(n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >> 24) & 0xFF)
end

local function readBlock(memType, size)
  local t = {}
  if memType == nil then
    for i = 1, size do t[i] = 0 end
  else
    for i = 0, size - 1 do t[i + 1] = emu.read(i, memType) end
  end
  return string.char(table.unpack(t))
end

local BUTTONS = { "a", "b", "select", "start", "up", "down", "left", "right" }
local function inputByte()
  local ok, inp = pcall(emu.getInput, 0)
  if not ok or type(inp) ~= "table" then return 0 end
  local b = 0
  for i, name in ipairs(BUTTONS) do
    if inp[name] then b = b | (1 << (i - 1)) end
  end
  return b
end

-- состояние процессора: pc, a, x, y, sp, ps, младшие 32 бита счётчика тактов
local function cpuBlock()
  local ok, st = pcall(emu.getState)
  if not ok or type(st) ~= "table" then return string.rep("\0", 12) end
  local function g(k) local v = st[k]; if type(v) ~= "number" then return 0 end; return math.floor(v) end
  if type(st.cpu) == "table" then
    local c = st.cpu
    st = { ["cpu.pc"] = c.pc, ["cpu.a"] = c.a, ["cpu.x"] = c.x, ["cpu.y"] = c.y,
           ["cpu.sp"] = c.sp, ["cpu.ps"] = c.ps, ["cpu.cycleCount"] = c.cycleCount }
  end
  return u16(g("cpu.pc")) .. string.char(g("cpu.a") & 0xFF, g("cpu.x") & 0xFF, g("cpu.y") & 0xFF,
         g("cpu.sp") & 0xFF, g("cpu.ps") & 0xFF, 0) .. u32(g("cpu.cycleCount") & 0xFFFFFFFF)
end

-- Запись: "F" | кадр (4) | кнопки (1) | флаги (1) | CPU (12) | RAM 2048 | OAM 256 | [nametable 2048]
-- флаги: бит0 = есть nametable, бит2 = первый кадр после перезагрузки
local frame = 0
local polled = nil
local resetDone = false
local resetMarkPending = false
local closed = false

if ET.inputPolled then
  emu.addEventCallback(function() polled = inputByte() end, ET.inputPolled)
end

emu.addEventCallback(function()
  if closed then return end
  frame = frame + 1
  local flags = 0
  local withNt = (frame % 4 == 0) and NT_T ~= nil
  if withNt then flags = flags | 1 end
  if resetMarkPending then flags = flags | 4; resetMarkPending = false end
  local inp = polled
  if inp == nil then inp = inputByte() end
  polled = nil
  bin:write("F", u32(frame), string.char(inp, flags), cpuBlock())
  bin:write(readBlock(RAM_T, 2048))
  bin:write(readBlock(OAM_T, 256))
  if withNt then bin:write(readBlock(NT_T, 2048)) end
  if frame % 60 == 0 then bin:flush() end

  if frame == 30 and not resetDone then
    local ok = pcall(emu.reset)
    if ok then
      resetDone = true
      resetMarkPending = true
      status = "REC"
    else
      status = "PRESS RESET (Game -> Reset)"
    end
  end
  emu.drawString(2, 2, status .. " " .. frame, 0xFF4040, 0xFF000000)
end, ET.endFrame)

if ET.reset then
  emu.addEventCallback(function()
    if not resetDone then
      resetDone = true
      resetMarkPending = true
      status = "REC"
    end
  end, ET.reset)
end

if ET.scriptEnded then
  emu.addEventCallback(function()
    if closed then return end
    closed = true
    bin:close()
  end, ET.scriptEnded)
end

emu.displayMessage("TGL", "Запись: " .. base .. ".bin")
