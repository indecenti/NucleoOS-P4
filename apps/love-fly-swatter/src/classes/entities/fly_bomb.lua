------------------------------------------------
--                 FLY SMALL (ENTITY)
-- fly_bomb object
------------------------------------------------

-- imports
local Entity = require('classes.entity');
local Vector2 = require('classes.math.vector2');
local Sprite = require('classes.graphics.sprite');
local Animation = require('classes.graphics.animation');

local Fly_Bomb = Entity:derive("Fly_Bomb");

-- fly_bomb constructor
function Fly_Bomb:new()
    self.position = Vector2(math.random(80, WIDTH-80), math.random(80, HEIGHT-80)); -- spawn fly_bomb random in the middle of the screen
    self.size = Vector2(16, 16);
    self.state = 1; -- 0: deactive, 1: active
    
    -- movement
    self.speed = math.random(60,80); -- entity speed
    self.angle = 0 -- angle in which direction the fly_bomb flies to

    -- timer
    self.timer = 0; -- timer
    self.timer_length = 1; -- how many seconds long the timer goes on till it resets

    -- sprite
    self.sprite_sheet = love.graphics.newImage("assets/graphics/sprite_sheets/fly_bomb.png");
    self.sprite = Sprite(self.sprite_sheet, self.size.x, self.size.y, self.position.x, self.position.y, 1, 1, 0);

    -- animation
    self.animation_fly_bomb = Animation(16, 16, 0, 2, 16);
    self.sprite:animation_add("fly_bomb", self.animation_fly_bomb);
    self.sprite:animate("fly_bomb");
end

-- fly_bomb update event
function Fly_Bomb:update(delta_time)
    if self.state == 1 then
        -- update sprite (animation)
        self.sprite:update(delta_time);
        self.sprite.position = self.position;

        -- movement
        self:move(delta_time);

        -- check if out of bounds when yes deactivate
        if (state == 0) then
                self:deactivate();
        end

        -- update timer
        self.timer = self.timer + delta_time;
        if(self.timer > self.timer_length) then self.timer = 0; end
    end
end

-- fly_bomb draw event
function Fly_Bomb:draw()

    if self.state == 1 then
        -- draw sprite
        self.sprite:draw();
    end
end

-- activate (change state to 1) fly_bomb
function Fly_Bomb:activate()
    -- activate
    self.state = 1;
end

-- deactivate (change state to 0) fly_bomb
function Fly_Bomb:deactivate()
    -- move out of bounds when deactivated
    self.position.x = WIDTH*2;
    self.position.y = HEIGHT*2;

    -- deactivate
    self.state = 0;
end

-- move fly_bomb into a random direction
function Fly_Bomb:move(delta_time)
    -- set random angle (every time the timer hits 0)
    if (self.timer == 0) then self.angle = math.random(-360,360); end
    -- move fly_bomb 
    self.position.x = (self.position.x + math.cos(self.angle) * self.speed * delta_time);
    self.position.y = (self.position.y + math.sin(self.angle) * self.speed * delta_time);
end

return Fly_Bomb