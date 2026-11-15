-- icon: tint
-- color: 3A86FF
-- Water counter: tap +1 for each glass. Goal 8 a day. Resets on a new day.
local GOAL = 8
local n = 0
local lbl, bar, note

local function today()
  local y, m, d = w.date()
  return y * 10000 + m * 100 + d
end

local function refresh()
  w.text(lbl, tostring(n) .. ' / ' .. GOAL)
  w.value(bar, math.min(n, GOAL))
  w.text(note, n >= GOAL and 'Goal reached!' or (GOAL - n) .. ' to go')
end

local function change(delta)
  n = math.max(0, n + delta)
  w.save('n', n)
  if delta > 0 then w.beep(1000, 80) end
  refresh()
end

function start()
  w.title('Water')
  if w.load('day', 0) ~= today() then
    w.save('day', today())
    w.save('n', 0)
  end
  n = w.load('n', 0)
  lbl = w.label('', nil, 170, 48, 0x3A86FF)
  bar = w.bar(nil, 250, 300, 18, GOAL)
  note = w.label('', nil, 305, 20)
  w.button('+1 glass', 110, 400, 170, 80, 0x3A86FF, function() change(1) end)
  w.button('-1', 310, 400, 110, 80, 0x555555, function() change(-1) end)
  refresh()
end

function on_button() return false end
function on_stop() end
