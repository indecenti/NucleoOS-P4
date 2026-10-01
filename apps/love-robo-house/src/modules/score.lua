-- Robo House - modules/score.lua
-- 2020 Foppygames

local score = {}

local aspect = require("modules.aspect")
local utils = require("modules.utils")

score.POINTS_FOR_BUTTON = 1
score.POINTS_FOR_COIN = 1

local MAX_SCORE = 999
local SCORE_LABEL_COLOR = utils.getColorFromRgb(131,118,156)
local SCORE_POINTS_COLOR = utils.getColorFromRgb(255,241,232)

local scorePoints = 0
local hiScorePoints = 0

function score.reset()
   scorePoints = 0 
end

function score.add(points)
    scorePoints = scorePoints + points
    if scorePoints > hiScorePoints then
        hiScorePoints = scorePoints
    end
 end

function score.draw()
    love.graphics.setColor(SCORE_LABEL_COLOR)
    love.graphics.print("SCORE",15,5)
    love.graphics.print("HI-SCORE",aspect.GAME_WIDTH-112,5)
    love.graphics.setColor(SCORE_POINTS_COLOR)
    love.graphics.print(scorePoints,70,5)
    love.graphics.printf(hiScorePoints,aspect.GAME_WIDTH-55,5,40,"right")
 end

return score