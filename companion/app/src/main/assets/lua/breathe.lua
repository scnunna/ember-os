-- icon: eye
-- color: 8E7DBE
-- Box breathing: breathe in 4, hold 4, out 4, hold 4. The ring changes colour with each step.
local STEPS = { 'Breathe in', 'Hold', 'Breathe out', 'Hold' }
local step, count, tid = 1, 4, nil
local circle, lbl, cnt

local function refresh()
  w.text(lbl, STEPS[step])
  w.text(cnt, tostring(count))
  local colors = { 0x6C9BD2, 0x8E7DBE, 0x4FB286, 0x8E7DBE }
  w.color(circle, colors[step])
end

local function tick()
  count = count - 1
  if count <= 0 then
    step = step % 4 + 1
    count = 4
    w.beep(700, 60)
  end
  refresh()
end

function start()
  w.title('Breathe')
  circle = w.circle(205, 270, 200, 0x8E7DBE, true)
  lbl = w.label(STEPS[1], nil, 130, 28)
  cnt = w.label('4', nil, 270, 48)
  tid = w.every(1000, tick)
end

function on_button() return false end
function on_stop() end
