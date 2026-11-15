-- icon: play
-- color: E4572E
-- Pomodoro: 25 minutes focus, 5 minutes break. Tap Start.
local FOCUS, REST = 25 * 60, 5 * 60
local mode, total, left, running = 'Focus', FOCUS, FOCUS, false
local endMs, tid
local lbl, state, bar, startBtn

local function fmt(s)
  return string.format('%02d:%02d', s // 60, s % 60)
end

local function refresh()
  w.text(lbl, fmt(left))
  w.text(state, mode)
  w.value(bar, 100 * (total - left) // total)
end

local function tick()
  left = math.ceil((endMs - w.millis()) / 1000)
  if left <= 0 then
    w.beep(880, 400)
    w.toast(mode .. ' done')
    if mode == 'Focus' then mode, total = 'Break', REST else mode, total = 'Focus', FOCUS end
    left = total
    running = false
    if tid then w.cancel(tid); tid = nil end
    w.text(startBtn, 'Start')
  end
  refresh()
end

local function toggle()
  if running then
    running = false
    if tid then w.cancel(tid); tid = nil end
    w.text(startBtn, 'Resume')
  else
    running = true
    endMs = w.millis() + left * 1000
    tid = w.every(1000, tick)
    w.text(startBtn, 'Pause')
  end
end

local function reset()
  running = false
  if tid then w.cancel(tid); tid = nil end
  mode, total, left = 'Focus', FOCUS, FOCUS
  w.text(startBtn, 'Start')
  refresh()
end

function start()
  w.title('Pomodoro')
  state = w.label(mode, nil, 120, 28, 0xE4572E)
  lbl = w.label(fmt(left), nil, 230, 48)
  bar = w.bar(nil, 300, 300, 16, 100)
  startBtn = w.button('Start', 110, 400, 160, 70, 0xE4572E, toggle)
  w.button('Reset', 300, 400, 160, 70, 0x555555, reset)
  refresh()
end

function on_button() return false end
function on_stop() end
