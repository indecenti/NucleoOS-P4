------------------------------------------------
--                  ANIMATION
-- WARNING: only use 1d spritesheets for animations
-- 'offset' variable is on the x axis
------------------------------------------------

-- imports
local Class = require('class');
local Vector2 = require('classes.math.vector2');

local Animation = Class:derive("Animation");

-- animation constructor
function Animation:new(width, height, offset, frame_max, fps)
    self.fps = fps;
    self.timer = 1 / self.fps;
    self.frame = 1;
    self.frame_max = frame_max;
    self.offset = offset;
    self.size = Vector2(width, height);
end

-- reset the animation
function Animation:reset()
    self.timer = 1 / self.fps;
    self.frame = 1;
end

-- apply animation to quad
function Animation:apply(quad)
    quad:setViewport(self.offset, 0, self.size.x, self.size.y);
end

-- update animation 
function Animation:update(delta_time, quad)
    -- if theres no animation (no max frames over 1) just dont bother with updating it
    if(self.frame_max <= 1) then return end 

    -- calculate animation 
    self.timer = self.timer - delta_time;
    if (self.timer <= 0) then
        self.timer = 1 / self.fps;
        self.frame = self.frame + 1;
        if (self.frame > self.frame_max) then
            self.frame = 1;
        end
        self.offset = (self.size.x * self.frame) - self.size.x;
        -- apply to quad
        self:apply(quad);
    end
end

return Animation;