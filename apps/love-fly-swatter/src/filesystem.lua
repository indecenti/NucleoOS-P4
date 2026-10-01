------------------------------------------------
--                   FILESYSTEM
--  everything to do wth the filesystem
------------------------------------------------

require('globals');

-- load highscore save file
function file_highscore_load() 
    -- check if highscore save file exists
    info = love.filesystem.getInfo("highscore.sav")
    if (info ~= nil) then
        -- file exists => read
        contents, size = love.filesystem.read("highscore.sav");
        HIGH_SCORE = tonumber(contents);
    else
        -- file doesnt exist => create
        file_highscore_write();
    end
end

-- write to highscore save
function file_highscore_write()
    success, message = love.filesystem.write( "highscore.sav", HIGH_SCORE);
    if (success == false) then
        -- file couldnt be written
        print("Error: Highscore file couldn't be written: " + message);
    end
end
