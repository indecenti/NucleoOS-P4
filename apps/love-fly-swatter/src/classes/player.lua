------------------------------------------------
--                  PLAYER
-- player object (cursor)
------------------------------------------------

-- imports
require('utils');
local Class = require('class');
local Vector2 = require('classes.math.vector2');
local Sprite = require('classes.graphics.sprite');
local Animation = require('classes.graphics.animation');

local Player = Class:derive("Player");

-- player constructor
function Player:new()
    self.position = Vector2(WIDTH/2, HEIGHT/2); -- center player at start of the room
    self.size = Vector2(16, 60);

    -- invincibility timer (invincibility means flys are invincible not player)
    self.invincibility_timer_max = 0.25;
    self.invincibility_timer = self.invincibility_timer_max;
    self.invincible = false;

    -- sprite
    self.sprite_sheet = love.graphics.newImage("assets/graphics/sprite_sheets/player.png");
    self.sprite = Sprite(self.sprite_sheet, self.size.x, self.size.y, self.position.x, self.position.y, 1, 1, 0);

    -- animation
    self.animation_hit = Animation(16, 60, 0, 2, 5);
    self.sprite:animation_add("hit", self.animation_hit);
    self.animation_idle = Animation(16, 60, 0, 1, 0);
    self.sprite:animation_add("idle", self.animation_idle);
end

-- player update event
function Player:update(delta_time)
    -- update sprite (animation)
        self.sprite:update(delta_time);
    -- check if on mobile or nahh
    if(OS ~= "Android" and OS ~= "iOS") then
        -- set player position to mouse position (additional calculations to make the mouse position relative to the screen size)
        self.position.x = (love.mouse.getX() * WIDTH) / love.graphics.getWidth();
        self.position.y = (love.mouse.getY() * HEIGHT) / love.graphics.getHeight();
    end

    -- self.invincibility
    if(self.invincible == true) then
      self.invincibility_timer = self.invincibility_timer - delta_time;
      if self.invincibility_timer <= 0 then
        self.invincibility_timer = self.invincibility_timer_max; -- reset timer
        self.invincible = false;
        self.sprite:animate("idle"); -- reset animation
      end
    end
end

-- player draw event
function Player:draw()
    -- set sprite position to position of player
    self.sprite.position.x = self.position.x;
    self.sprite.position.y = self.position.y;

    -- draw sprite
    self.sprite:draw();
end

-- mouse pressed event
function Player:mousepressed(x, y, button, istouch, presses, level)
    --[[
        NOTE: i got an idea to use the 'presses' variable for maybe sum kinda special attack when screen is double clicked / tapped
    ]]--
    if self.invincible == false then
      -- handle mouse press
      -- go through every entity in list of level and check if its fly
      for i = 1, table.getn(level.entities), 1 do
        -- is it normal fly?
        if level.entities[i]:get_type() == "Fly" then
          -- calculate position of fly and accomidate screen size
          x = level.entities[i].position.x * (love.graphics.getWidth() / WIDTH);
          y = level.entities[i].position.y * (love.graphics.getHeight() / HEIGHT);
          -- check if mouse if over fly when yes destroy fly and add score (24 is the size of the hitbox)
          if get_distance(x, y, love.mouse.getX(), love.mouse.getY()) < 24 * (love.graphics.getWidth()/love.graphics.getHeight()) then
            self:enemy_killed_event(level.entities[i], 1);
          end
          -- is it small fly?
        elseif level.entities[i]:get_type() == "Fly_Small" then
          -- calculate position of small fly and accomidate screen size
          x = level.entities[i].position.x * (love.graphics.getWidth() / WIDTH);
          y = level.entities[i].position.y * (love.graphics.getHeight() / HEIGHT);
          -- check if mouse if over small fly when yes destroy fly and add score (24 is the size of the hitbox)
          if get_distance(x, y, love.mouse.getX(), love.mouse.getY()) < 24 * (love.graphics.getWidth()/love.graphics.getHeight()) then
            self:enemy_killed_event(level.entities[i], 3);
          end
          -- its bomb fly
        else
          -- calculate position of bomb fly and accomidate screen size
          x = level.entities[i].position.x * (love.graphics.getWidth() / WIDTH);
          y = level.entities[i].position.y * (love.graphics.getHeight() / HEIGHT);
          -- check if mouse if over bomb fly when yes destroy fly and add score (24 is the size of the hitbox)
          if get_distance(x, y, love.mouse.getX(), love.mouse.getY()) < 24 * (love.graphics.getWidth()/love.graphics.getHeight()) then
            -- deactivate bomb fly
            level.entities[i]:deactivate(); -- deactivate entity
            -- player dies => move to gameover screen
            game_over();
          end
        end
      end
    end

    -- if touch move player to current press position
    if istouch then
        -- set player position to press position (additional calculations to make the mouse position relative to the screen size)
        self.position.x = (x * WIDTH) / love.graphics.getWidth();
        self.position.y = (y * HEIGHT) / love.graphics.getHeight();
    end
end

-- gets run when an enemy is killed and takes in entity and the score to add
function Player:enemy_killed_event(entity, score)
            CURRENT_SCORE = CURRENT_SCORE + score; -- add score
            entity:deactivate(); -- deactivate entity
            self.invincible = true; -- self.invincibility of other flys activateeeddd!
            self.sprite:animate("hit"); -- animate hitting animation
end

return Player;
