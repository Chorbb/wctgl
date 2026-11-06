-- tgl_logger2.lua — запись прохождения The Guardian Legend С ВКЛЮЧЕНИЯ, для сверки порта.
--
-- При запуске скрипт перезагружает игру (reset) и пишет:
--   * снимок памяти в момент перезагрузки (кадр 0)
--   * каждый кадр: кнопки, которые игра реально прочитала, RAM (2 КБ), OAM (256 Б)
--   * раз в 4 кадра: память фона (nametable, 2 КБ)
-- По этой записи эмулятор порта повторяет прохождение и сверяет память кадр за кадром.
--
-- Нужно: Settings -> "Allow access to I/O and OS functions".

local MT = emu.memType
local RAM_T = MT.nesInternalRam or MT.nesDebug or MT.cpuDebug
local OAM_T = MT.nesSpriteRam or MT.spriteRam or MT.oam
local NT_T  = MT.nesNametableRam or MT.nametableRam

local folder = emu.getScriptDataFolder()
if folder == nil or folder == "" then
  emu.displayMessage("TGL", "Нет доступа к файлам: Settings -> Allow access to I/O and OS functions")
  emu.addEventCallback(function()
    emu.drawString(4, 4, "TGL LOGGER: I/O ACCESS DISABLED", 0xFF4040, 0xFF000000)
  end, emu.eventType.endFrame)
  return
end
if not folder:match("[/\\]$") then folder = folder .. "\\" end

local stamp = os.date("%Y%m%d_%H%M%S")
local base = folder .. "run_" .. stamp
local bin = io.open(base .. ".bin", "wb")
bin:write("TGLLOG2\n")

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

-- Формат записи: "F" | кадр (4) | кнопки (1) | флаги (1: бит0 = есть nametable, бит1 = снимок reset)
--                | RAM 2048 | OAM 256 | [nametable 2048]
local frame = -1          -- -1 = ждём перезагрузки
local polled = 0
local closed = false

local function writeRecord(flags, withNt)
  bin:write("F", u32(math.max(frame, 0)), string.char(polled, flags | (withNt and 1 or 0)))
  bin:write(readBlock(RAM_T, 2048))
  bin:write(readBlock(OAM_T, 256))
  if withNt then bin:write(readBlock(NT_T, 2048)) end
end

-- кнопки берём в тот момент, когда игра их опрашивает
emu.addEventCallback(function()
  polled = inputByte()
end, emu.eventType.inputPolled)

emu.addEventCallback(function()
  if closed then return end
  frame = 0
  polled = 0
  writeRecord(2, false)   -- снимок памяти в момент перезагрузки
end, emu.eventType.reset)

emu.addEventCallback(function()
  if closed or frame < 0 then return end
  frame = frame + 1
  local withNt = (frame % 4 == 0) and NT_T ~= nil
  writeRecord(0, withNt)
  if frame % 60 == 0 then bin:flush() end
  emu.drawString(2, 2, "REC " .. frame, 0xFF4040, 0xFF000000)
end, emu.eventType.endFrame)

emu.addEventCallback(function()
  if closed then return end
  closed = true
  bin:close()
end, emu.eventType.scriptEnded)

-- перезагружаем игру, чтобы запись началась с самого начала
local ok = pcall(emu.reset)
if not ok then
  emu.displayMessage("TGL", "Не удалось перезагрузить: нажмите Reset вручную (Game -> Reset)")
else
  emu.displayMessage("TGL", "Запись с перезагрузки: " .. base .. ".bin")
end
