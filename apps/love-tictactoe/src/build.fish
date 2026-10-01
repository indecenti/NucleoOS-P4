#!/usr/bin/env fish

# Change to the path of love.exe and dll files
# If you don't have it you can download here: https://love2d.org/
# (Download the 64-bit zip and extract the files)
set love_exe_path $HOME/Downloads/love-11.5-win64/

if not test -d $love_exe_path
    echo 'The love_exe_path does not exist!'
    echo 'Edit the script and change the love_exe_path variable.'
    exit 1
end

if test -d 'dist/'
    echo 'Removing existing dist files...'
    rm -rf 'dist/'
    echo 'Done!'
end

echo 'Making distribution directory...'
mkdir -p 'dist/tmp'
echo 'Done!'

set filename (status current-filename)

echo 'Building .love package...'
zip -r -9 dist/tictactoe.love . -x $filename -x ".git/*" -x .gitignore
echo 'Done!'

echo 'Copying .dll and license files...'
cp $love_exe_path/*.dll $love_exe_path/license.txt 'dist/tmp'
echo 'Done!'

echo 'Building tictactoe.exe'
cat $love_exe_path/love.exe './dist/tictactoe.love' > 'dist/tmp/tictactoe.exe'
echo 'Done!'

echo 'Compressing to windows-64 zip...'
cd 'dist/tmp'
zip -r '../tictactoe_win64.zip' .
cd -
rm -rf 'dist/tmp'
echo 'Done!\n'

echo 'BUILDING DONE! Game is ready for distribution.'
