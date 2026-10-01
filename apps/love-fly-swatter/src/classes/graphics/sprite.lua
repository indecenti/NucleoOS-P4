------------------------------------------------
--                  SPRITE
-- a simple sprite with different attributes and
-- customizable animations
------------------------------------------------

-- imports 
local Class = require('class');
local Vector2 = require('classes.math.vector2');

local Sprite = Class:derive("Sprite");

-- sprite constructor
function Sprite:new(sprite_sheet, width, height, x, y, scale_x, scale_y, angle, origin_x, origin_y)
    self.size = Vector2(width, height)
    self.position = Vector2(x, y);
    self.scale = Vector2(scale_x or 1, scale_y or 1);
    self.angle = angle or 0;
    self.origin = Vector2(origin_x or 0, origin_y or 0);

    self.sprite_sheet = sprite_sheet;
    self.quad = love.graphics.newQuad(0, 0, self.size.x, self.size.y, sprite_sheet:getDimensions());

    self.animations = {};
    self.animation_current = "";
end

-- sprite update function 
function Sprite:update(delta_time)
    -- if theres an animation play it
    if (self.animations[self.animation_current] ~= nil) then
        self.animations[self.animation_current]:update(delta_time, self.quad);
    end
end

-- sprite draw function
function Sprite:draw()
    love.graphics.draw(self.sprite_sheet, self.quad, self.position.x, self.position.y, self.angle, self.scale.x, self.scale.y, self.size.x / 2, self.size.y / 2, self.origin.x, self.origin.y);
end


------------------------------------------------
--                ANIMATIONS
------------------------------------------------
function Sprite:animate(animation_name)
    if (self.animation_current ~= animation_name and self.animations[animation_name] ~= nil) then
        self.animation_current = animation_name;
        self.animations[animation_name]:reset();
        self.animations[animation_name]:apply(self.quad);
    end
end

function Sprite:animation_add(name, animation)
    self.animations[name] = animation;
end

return Sprite;