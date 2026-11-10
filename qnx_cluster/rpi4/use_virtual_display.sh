#!/bin/sh
# 부팅 때 HDMI 모니터가 없으면 HDMI0 대신 가상 디스플레이(1920x1080)를 쓰게 한다. VNC로 화면을 본다.
# 모니터가 연결돼 있으면 지금처럼 HDMI 설정을 쓴다.
# Screen은 가상 디스플레이에 ID 1024를 준다(HDMI는 1). vncserv도 그 ID를 읽게 한다.
# 되돌리기: sudo cp -p /usr/etc/startup/post_startup.sh.orig /usr/etc/startup/post_startup.sh
set -e
F=/usr/etc/startup/post_startup.sh
[ -f "$F.orig" ] || cp -p "$F" "$F.orig"
sed -e 's/^        echo "WARNING: Unable to detect a display. Assuming one plugged into HDMI0"$/        echo "No display detected: using the virtual display (VNC)"/' \
    -e 's/^        _screenCfg="graphics-rpi5-hdmi1.conf"$/        _screenCfg="graphics-rpi5-virtual.conf"; _vncDisplay=1024/' \
    -e 's/^vncserv -display=1 -auth$/vncserv -display=${_vncDisplay:-1} -auth/' \
    "$F.orig" > "$F.new"
chmod 555 "$F.new"
mv "$F.new" "$F"
echo "--- 바뀐 줄:"
diff "$F.orig" "$F" || true
echo "완료. 'sudo shutdown' 으로 재부팅하세요."
