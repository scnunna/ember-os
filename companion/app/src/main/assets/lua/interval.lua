-- icon: loop
-- color: F4A261
-- Interval timer: 30 s work, 10 s rest, 8 rounds. Beeps at every change.
local WORK, REST, ROUNDS = 30, 10, 8
local phase, round, left, running = 'Ready', 1, WORK, false
local endMs, tid
local lbl, state, roundLbl, startBtn

local function refresh()
  w.text(lbl, tostring(left))
  w.text(state, phase)
  w.text(roundLbl, 'Round ' .. round .. ' / ' .. ROUNDS)
  w.color(state, phase == 'Rest' and 0x3A86FF or 0xF4A261)
end

local function stop_timer()
  running = false
  if tid then w.cancel(tid); tid = nil end
  w.text(startBtn, 'Start')
end

local function next_phase()
  if phase == 'Work' then
    phase, left = 'Rest', REST
  else
    if phase == 'Rest' then round = round + 1 end
    if round > ROUNDS then
      stop_timer()
      phase, round, left = 'Done', ROUNDS, 0
      w.beep(660, 600)
      refresh()
      return
    end
    phase, left = 'Work', WORK
  end
  endMs = w.millis() + left * 1000
  w.beep(phase == 'Work' and 1000 or 500, 250)
end

local function tick()
  left = math.ceil((endMs - w.millis()) / 1000)
  if left <= 0 then next_phase() end
  refresh()
end

local function toggle()
  if running then stop_timer(); return end
  if phase == 'Ready' or phase == 'Done' then
    phase, round, left = 'Work', 1, WORK
  end
  running = true
  endMs = w.millis() + left * 1000
  tid = w.every(250, tick)
  w.text(startBtn, 'Stop')
  refresh()
end

function start()
  w.title('Intervals')
  state = w.label(phase, nil, 120, 28)
  lbl = w.label(tostring(left), nil, 230, 48)
  roundLbl = w.label('', nil, 320, 20)
  startBtn = w.button('Start', nil, 420, 220, 80, 0xF4A261, toggle)
  refresh()
end

function on_button() return false end
function on_stop() end
