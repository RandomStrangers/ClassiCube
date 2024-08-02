DESKTOP_FILE=Zeta.desktop
GAME_DIR=`pwd`

# remove existing Zeta Core desktop entry file
rm $DESKTOP_FILE

# download Core icon from github if necessary
if [ -f "Zicon.png" ]
then
  echo "Zicon.png exists already. Skipping download."
else
  echo "Zicon.png doesn't exist. Attempting to download it.."
  wget "https://cdn.discordapp.com/attachments/1167261493725765654/1187201470181941338/image.png"
fi

# create Zeta Core desktop entry
echo 'Creating Zeta.desktop..'
cat >> $DESKTOP_FILE << EOF
[Desktop Entry]
Type=Application
Comment=Minecraft Classic inspired sandbox game
Name=Zeta
Exec=$GAME_DIR/Zeta
Icon=$GAME_DIR/Zicon.png
Path=$GAME_DIR
Terminal=false
Categories=Game
EOF
chmod +x $DESKTOP_FILE

echo 'Installing Zeta.desktop..'
# install Zeta Core desktop entry into the system
sudo desktop-file-install --dir=/usr/share/applications Zeta.desktop
sudo update-desktop-database /usr/share/applications
