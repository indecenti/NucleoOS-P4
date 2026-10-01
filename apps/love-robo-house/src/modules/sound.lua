-- Robo House - modules/sound.lua
-- 2020 Foppygames

local sound = {}

sound.SOUND_JUMP = 1
sound.SOUND_CLIMB = 2
sound.SOUND_BUTTON_NEUTRAL = 3
sound.SOUND_BUTTON_SUCCESS = 4
sound.SOUND_PLAYER_HIT = 5
sound.SOUND_PLAYER_HURT = 6
sound.SOUND_KITTEN_HURT = 7
sound.SOUND_ROBOT_ATTACK = 8
sound.SOUND_COIN = 9

local VOLUME = 0.1

local sources = {}

function sound.init()
    sources[sound.SOUND_JUMP] = love.audio.newSource("/sound/jump.wav","static")
    sources[sound.SOUND_CLIMB] = love.audio.newSource("/sound/climb.wav","static")
    sources[sound.SOUND_BUTTON_NEUTRAL] = love.audio.newSource("/sound/button_neutral.wav","static")
    sources[sound.SOUND_BUTTON_SUCCESS] = love.audio.newSource("/sound/button_success.wav","static")
    sources[sound.SOUND_PLAYER_HIT] = love.audio.newSource("/sound/player_hit.wav","static")
    sources[sound.SOUND_PLAYER_HURT] = love.audio.newSource("/sound/player_hurt.wav","static")
    sources[sound.SOUND_KITTEN_HURT] = love.audio.newSource("/sound/kitten_hurt.wav","static")
    sources[sound.SOUND_ROBOT_ATTACK] = love.audio.newSource("/sound/robot_attack.wav","static")
    sources[sound.SOUND_COIN] = love.audio.newSource("/sound/coin.wav","static")
    for i = 1, #sources do
        sources[i]:setVolume(VOLUME)
    end
end

function sound.play(index,pitch)
    if pitch ~= nil then
        sources[index]:setPitch(pitch)
    end
    love.audio.play(sources[index])
end

return sound