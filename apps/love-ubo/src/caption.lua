Caption = Mobile:subclass('Caption')

Caption.static.color   = {1,1,1}
Caption.static.activeColor = {1,1,0}
Caption.static.bgColor     = {.5,.5,.5,.5}

function Caption:initialize(font)

	self.font=font
	self:setLines()

end

function Caption:setLines(tbl)
	if not tbl then
		self.lines={}
	else
		self.lines=tbl
	end
	if #self.lines == 0  then
		self.h = self.font:getHeight()
		self.w = 100
	else
		self.w = 100
		self.h = (1 + 2*#self.lines) * self.font:getHeight() 
		for i=1,#self.lines do
			local wl = self.font:getWidth(self.lines[i]) + 50
			if wl > self.w then
				self.w = wl
			end
		end
	end
	
	self:center()

	self.active=0
	self.selected=0
end

function Caption:center()
	self.x = math.floor((love.graphics.getWidth()- self.w)/2)
	self.y = math.floor((love.graphics.getHeight() - self.h)/2)
end

function Caption:setMenuMode(menuMode) 
	if menuMode then
		self.active=1
		self.selected=0
	else
		self.active=0
		self.selected=0
	end
end

function Caption:draw()
	love.graphics.setColor(Caption.bgColor)
	love.graphics.rectangle('fill',self.x,self.y,self.w,self.h)
	love.graphics.setFont(self.font)
	for i,text in ipairs(self.lines) do
		if self.active == i then
			love.graphics.setColor(Caption.activeColor)
		else
			love.graphics.setColor(Caption.color)
		end
		love.graphics.printf(text,self.x,self.y + (2*i-1) * self.font:getHeight(),self.w,"center")	
	end
end

function Caption:keypressed(key)
	if self.selected == 0 then
		if key == "up" then
			self.active = self.active - 1
			if self.active < 1 then
				self.active=#self.lines
			end
		elseif key == "down" then
			self.active = self.active + 1
			if self.active > #self.lines then
				self.active=1
			end
		elseif key == " " or key == "return" then
			self.selected = self.active
		end
	end
end

function Caption:mousepressed(x,y,b)
	if self.selected ~= 0 then
		return
	end
	if x < self.x or y < self.y or x > self.x + self.w or y > self.y + self.h then
		return
	end
	
	local row = math.floor((y-self.y) / self.font:getHeight())
	if row % 2 == 0 then
		return
	end
	
	local lid =  (row+1)/2
	if lid < 1 or lid > #self.lines then
		return
	end
	
	local ox = math.abs(x - self.x - self.w/2)
	if ox <= self.font:getWidth(self.lines[lid])/2 then
		self.active = lid
		self.selected = lid
	end
	
end