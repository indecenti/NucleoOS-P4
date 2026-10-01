-- Robo House - modules/images.lua
-- 2020 Foppygames

local images = {}

images.IMAGE_COIN_1 = "coin_1"
images.IMAGE_KITTEN_ATTACKED_LEFT = "kitten_attacked_left"
images.IMAGE_KITTEN_ATTACKED_RIGHT = "kitten_attacked_right"
images.IMAGE_KITTEN_SIT_LEFT = "kitten_sit_left"
images.IMAGE_KITTEN_SIT_RIGHT = "kitten_sit_right"
images.IMAGE_KITTEN_WALK_LEFT_1 = "kitten_walk_left_1"
images.IMAGE_KITTEN_WALK_LEFT_2 = "kitten_walk_left_2"
images.IMAGE_KITTEN_WALK_RIGHT_1 = "kitten_walk_RIGHT_1"
images.IMAGE_KITTEN_WALK_RIGHT_2 = "kitten_walk_RIGHT_2"
images.IMAGE_PLAYER_EXPLOSION_1 = "player_explosion_1"
images.IMAGE_PLAYER_EXPLOSION_2 = "player_explosion_2"
images.IMAGE_PLAYER_EXPLOSION_3 = "player_explosion_3"
images.IMAGE_PLAYER_JUMP_LEFT = "player_jump_left"
images.IMAGE_PLAYER_JUMP_RIGHT = "player_jump_right"
images.IMAGE_PLAYER_WALK_LEFT_1 = "player_walk_left_1"
images.IMAGE_PLAYER_WALK_RIGHT_1 = "player_walk_right_1"
images.IMAGE_PLAYER_WALK_LEFT_2 = "player_walk_left_2"
images.IMAGE_PLAYER_WALK_RIGHT_2 = "player_walk_right_2"
images.IMAGE_ROBOT_ATTACK_LEFT_1 = "robot_attack_left_1"
images.IMAGE_ROBOT_ATTACK_LEFT_2 = "robot_attack_left_2"
images.IMAGE_ROBOT_ATTACK_RIGHT_1 = "robot_attack_right_1"
images.IMAGE_ROBOT_ATTACK_RIGHT_2 = "robot_attack_right_2"
images.IMAGE_ROBOT_WALK_LEFT_1 = "robot_walk_left_1"
images.IMAGE_ROBOT_WALK_RIGHT_1 = "robot_walk_right_1"

local list = {}

function images.init()
    list[images.IMAGE_COIN_1] = {image = love.graphics.newImage("images/coin_1.png")}
    list[images.IMAGE_KITTEN_ATTACKED_LEFT] = {image = love.graphics.newImage("images/kitten_attacked_left.png")}
    list[images.IMAGE_KITTEN_ATTACKED_RIGHT] = {image = love.graphics.newImage("images/kitten_attacked_right.png")}
    list[images.IMAGE_KITTEN_SIT_LEFT] = {image = love.graphics.newImage("images/kitten_sit_left.png")}
    list[images.IMAGE_KITTEN_SIT_RIGHT] = {image = love.graphics.newImage("images/kitten_sit_right.png")}
    list[images.IMAGE_KITTEN_WALK_LEFT_1] = {image = love.graphics.newImage("images/kitten_walk_left_1.png")}
    list[images.IMAGE_KITTEN_WALK_LEFT_2] = {image = love.graphics.newImage("images/kitten_walk_left_2.png")}
    list[images.IMAGE_KITTEN_WALK_RIGHT_1] = {image = love.graphics.newImage("images/kitten_walk_right_1.png")}
    list[images.IMAGE_KITTEN_WALK_RIGHT_2] = {image = love.graphics.newImage("images/kitten_walk_right_2.png")}
    list[images.IMAGE_PLAYER_EXPLOSION_1] = {image = love.graphics.newImage("images/player_explosion_1.png")}
    list[images.IMAGE_PLAYER_EXPLOSION_2] = {image = love.graphics.newImage("images/player_explosion_2.png")}
    list[images.IMAGE_PLAYER_EXPLOSION_3] = {image = love.graphics.newImage("images/player_explosion_3.png")}
    list[images.IMAGE_PLAYER_JUMP_LEFT] = {image = love.graphics.newImage("images/player_jump_left.png")}
    list[images.IMAGE_PLAYER_JUMP_RIGHT] = {image = love.graphics.newImage("images/player_jump_right.png")}
    list[images.IMAGE_PLAYER_WALK_LEFT_1] = {image = love.graphics.newImage("images/player_walk_left.png")}
    list[images.IMAGE_PLAYER_WALK_LEFT_2] = {image = love.graphics.newImage("images/player_walk_left_2.png")}
    list[images.IMAGE_PLAYER_WALK_RIGHT_1] = {image = love.graphics.newImage("images/player_walk_right.png")}
    list[images.IMAGE_PLAYER_WALK_RIGHT_2] = {image = love.graphics.newImage("images/player_walk_right_2.png")}
    list[images.IMAGE_ROBOT_ATTACK_LEFT_1] = {image = love.graphics.newImage("images/robot_attack_left_1.png")}
    list[images.IMAGE_ROBOT_ATTACK_LEFT_2] = {image = love.graphics.newImage("images/robot_attack_left_2.png")}
    list[images.IMAGE_ROBOT_ATTACK_RIGHT_1] = {image = love.graphics.newImage("images/robot_attack_right_1.png")}
    list[images.IMAGE_ROBOT_ATTACK_RIGHT_2] = {image = love.graphics.newImage("images/robot_attack_right_2.png")}
    list[images.IMAGE_ROBOT_WALK_LEFT_1] = {image = love.graphics.newImage("images/robot_walk_left_1.png")}
    list[images.IMAGE_ROBOT_WALK_RIGHT_1] = {image = love.graphics.newImage("images/robot_walk_right_1.png")}
    
    for key, _ in pairs(list) do
        list[key]["w"] = list[key]["image"]:getWidth()
        list[key]["h"] = list[key]["image"]:getHeight()
    end
end

function images.get(key)
    return list[key]
end

return images