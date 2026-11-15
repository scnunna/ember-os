-- icon: ok
-- color: 2EC4B6
-- Habit tick: three daily habits. Tap one to tick it. Resets every new day and counts your streak.
local NAMES = { 'Move', 'Read', 'Sleep early' }
local btns, done = {}, {}
local streakLbl

local function today()
  local y, m, d = w.date()
  return y * 10000 + m * 100 + d
end

local function load_state()
  if w.load('day', 0) ~= today() then
    local all = true
    for i = 1, #NAMES do if w.load('h' .. i, 0) ~= 1 then all = false end end
    local streak = w.load('streak', 0)
    if all then streak = streak + 1 else streak = 0 end
    w.save('streak', streak)
    w.save('day', today())
    for i = 1, #NAMES do w.save('h' .. i, 0) end
  end
  for i = 1, #NAMES do done[i] = w.load('h' .. i, 0) == 1 end
end

local function paint(i)
  w.text(btns[i], (done[i] and 'x  ' or '   ') .. NAMES[i])
  w.color(btns[i], done[i] and 0x2EC4B6 or 0x444444)
end

local function tap(i)
  done[i] = not done[i]
  w.save('h' .. i, done[i] and 1 or 0)
  paint(i)
  w.beep(done[i] and 1200 or 600, 60)
end

function start()
  w.title('Habits')
  load_state()
  streakLbl = w.label('Streak: ' .. w.load('streak', 0) .. ' days', nil, 110, 20)
  for i = 1, #NAMES do
    btns[i] = w.button(NAMES[i], nil, 130 + i * 90, 300, 70, 0x444444, function() tap(i) end)
    paint(i)
  end
end

function on_button() return false end
function on_stop() end
