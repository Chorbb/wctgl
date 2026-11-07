-- tgl_logger5.lua — запись прохождения The Guardian Legend для перевода кода (Mesen 2).
--
-- Пишет два файла в папку данных скрипта:
--   cover_<время>.txt  — какие инструкции игра реально исполнила (адрес и банк) и куда
--                        приводили RTS. По нему переводчик решает, что переводить.
--   snap_<время>.bin   — нажатия кнопок каждого кадра и раз в ~20 секунд полный снимок
--                        приставки (память, видеопамять, регистры). С этих точек наш
--                        эмулятор сам проигрывает игру и проверяет перевод.
--
-- Перезагрузка не нужна: можно начинать с любого места (в т.ч. с сохранения Mesen).
-- Сверху экрана: число найденных инструкций и снимков. Файлы дописываются по ходу игры,
-- так что если Mesen закроется — записанное до этого момента сохранится.
--
-- Нужно: Settings -> "Allow access to I/O and OS functions".

local MT = emu.memType
local ET = emu.eventType
local CT = emu.callbackType or emu.memCallbackType
local EXEC = CT and (CT.exec or CT.cpuExec)
local WRITE = CT and (CT.write or CT.cpuWrite)
local CPU_T = emu.cpuType and (emu.cpuType.nes or emu.cpuType.cpu)

local RAM_T = MT.nesInternalRam
local OAM_T = MT.nesSpriteRam
local NT_T  = MT.nesNametableRam
local CHR_T = MT.nesChrRam
local PAL_T = MT.nesPaletteRam

-- запасной путь: всё адресное пространство видеочипа ($0000-$3FFF)
local PPU_T = MT.nesPpuMemory or MT.ppuMemory
local sources = {}
local function pick(direct, name, ppu_offset)
  if direct ~= nil then sources[#sources + 1] = name .. "=direct"; return direct, 0 end
  if PPU_T ~= nil then sources[#sources + 1] = name .. "=ppu"; return PPU_T, ppu_offset end
  return nil, 0
end
local NT_SRC, NT_OFS = pick(NT_T, "nt", 0x2000)
local CHR_SRC, CHR_OFS = pick(CHR_T, "chr", 0x0000)
local PAL_SRC, PAL_OFS = pick(PAL_T, "pal", 0x3F00)

local problem = nil
if EXEC == nil or WRITE == nil then problem = "NO EXEC/WRITE CALLBACKS" end
if RAM_T == nil then problem = "NO RAM MEMTYPE" end
if OAM_T == nil then problem = "NO OAM MEMTYPE" end
if NT_SRC == nil or CHR_SRC == nil or PAL_SRC == nil then problem = "NO PPU MEMTYPE" end

local folder = emu.getScriptDataFolder()
if folder == nil or folder == "" then problem = "I/O ACCESS DISABLED" end

if problem then
  emu.addEventCallback(function()
    emu.drawString(4, 4, "TGL LOGGER5: " .. problem, 0xFF4040, 0xFF000000)
  end, ET.endFrame)
  return
end
if not folder:match("[/\\]$") then folder = folder .. "\\" end

local stamp = os.date("%Y%m%d_%H%M%S")
local cover_path = folder .. "cover_" .. stamp .. ".txt"
local snap = io.open(folder .. "snap_" .. stamp .. ".bin", "wb")
snap:write("TGLSNAP1\n")

local function u16(n) return string.char(n & 0xFF, (n >> 8) & 0xFF) end
local function u32(n)
  return string.char(n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, (n >> 24) & 0xFF)
end

local function readBlock(memType, size, offset)
  offset = offset or 0
  local parts, t = {}, {}
  for i = 0, size - 1 do
    t[#t + 1] = emu.read(offset + i, memType)
    if #t == 256 then parts[#parts + 1] = string.char(table.unpack(t)); t = {} end
  end
  if #t > 0 then parts[#parts + 1] = string.char(table.unpack(t)) end
  return table.concat(parts)
end

-- ---------------------------------------------------------------- покрытие кода
-- банк $8000-$BFFF: UNROM переключается записью в $8000-$FFFF (младшие 3 бита)
local bank = 0
emu.addMemoryCallback(function(addr, value)
  bank = value & 7
end, WRITE, 0x8000, 0xFFFF)

local seen = {}       -- ключ: банк * 65536 + адрес
local rts_to = {}
local count = 0
local last_rts = false

emu.addMemoryCallback(function(addr, value)
  local b = (addr >= 0xC000) and 7 or bank
  local k = b * 65536 + addr
  if not seen[k] then seen[k] = true; count = count + 1 end
  if last_rts and not rts_to[k] then rts_to[k] = true end
  last_rts = (value == 0x60)
end, EXEC, 0x8000, 0xFFFF)

local function save_cover()
  local f = io.open(cover_path, "w")
  if f == nil then return end
  local lines = {}
  for k, _ in pairs(seen) do lines[#lines + 1] = string.format("C %d %04X", k >> 16, k & 0xFFFF) end
  for k, _ in pairs(rts_to) do lines[#lines + 1] = string.format("T %d %04X", k >> 16, k & 0xFFFF) end
  table.sort(lines)
  f:write(table.concat(lines, "\n"), "\n")
  f:close()
end

-- ---------------------------------------------------------------- состояние для снимка
-- всё, что отдаёт emu.getState, — текстом «ключ=значение» (разберём на стороне порта)
local function flatten(t, prefix, out)
  for k, v in pairs(t) do
    local key = prefix .. tostring(k)
    if type(v) == "table" then flatten(v, key .. ".", out)
    elseif type(v) == "number" or type(v) == "boolean" then out[#out + 1] = key .. "=" .. tostring(v) end
  end
  return out
end

-- Снимок берём на входе в обработчик NMI ($DC75): это граница инструкции,
-- а в стеке уже лежит адрес возврата — порт начнёт ровно отсюда.
local frame = 0
local snaps = 0
local want_snap = true
local SNAP_EVERY = 1200        -- кадров (~20 секунд)
local last_snap_frame = -SNAP_EVERY

emu.addMemoryCallback(function(addr, value)
  if not want_snap then return end
  want_snap = false
  last_snap_frame = frame
  local ok, st = pcall(emu.getState)
  local text = (ok and type(st) == "table") and table.concat(flatten(st, "", {}), "\n") or ""
  text = text .. "\nlogger.bank=" .. bank .. "\nlogger.frame=" .. frame .. "\nlogger.sources=" .. table.concat(sources, ",")
  -- "S" | кадр (4) | банк (1) | длина текста (4) | текст | RAM 2048 | OAM 256 | NT 2048 | CHR 8192 | PAL 32
  snap:write("S", u32(frame), string.char(bank), u32(#text), text)
  snap:write(readBlock(RAM_T, 2048), readBlock(OAM_T, 256), readBlock(NT_SRC, 2048, NT_OFS),
             readBlock(CHR_SRC, 8192, CHR_OFS), readBlock(PAL_SRC, 32, PAL_OFS))
  snap:flush()
  snaps = snaps + 1
end, EXEC, 0xDC75, 0xDC75)

-- ---------------------------------------------------------------- кнопки каждого кадра
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

local polled = nil
if ET.inputPolled then
  emu.addEventCallback(function() polled = inputByte() end, ET.inputPolled)
end

local closed = false
emu.addEventCallback(function()
  if closed then return end
  frame = frame + 1
  local inp = polled
  if inp == nil then inp = inputByte() end
  polled = nil
  -- "I" | кадр (4) | кнопки (1)
  snap:write("I", u32(frame), string.char(inp))
  if frame - last_snap_frame >= SNAP_EVERY then want_snap = true end
  if frame % 600 == 0 then save_cover(); snap:flush() end
  emu.drawString(2, 2, string.format("TGL REC  code %d  snaps %d", count, snaps), 0x40FF40, 0xFF000000)
end, ET.endFrame)

if ET.scriptEnded then
  emu.addEventCallback(function()
    if closed then return end
    closed = true
    save_cover()
    snap:close()
  end, ET.scriptEnded)
end

emu.displayMessage("TGL", "Запись: " .. folder)
