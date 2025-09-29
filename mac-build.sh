mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF 
cmake --build .
rm ZipCombiner.app/Contents/Info.plist
cp ../mac/Info.plist ZipCombiner.app/Contents/
cp -R ZipCombiner.app ~/Desktop

