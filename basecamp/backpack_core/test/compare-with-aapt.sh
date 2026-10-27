BT=$(ls -d ~/Android/Sdk/build-tools/*/ | sort -V | tail -1)
for f in "$@"; do
  mine=$(./at "$f")
  b=$($BT/aapt2 dump badging "$f" 2>/dev/null)
  id=$(echo "$b" | grep -o "package: name='[^']*'" | cut -d"'" -f2); vc=$(echo "$b" | grep -o "versionCode='[0-9]*'" | cut -d"'" -f2)
  vn=$(echo "$b" | grep -o "versionName='[^']*'" | head -1 | cut -d"'" -f2); lab=$(echo "$b" | grep -o "application-label:'[^']*'" | head -1 | cut -d"'" -f2)
  ms=$(echo "$b" | grep -o "minSdkVersion:'[0-9]*'" | cut -d"'" -f2)
  sg=$($BT/apksigner verify --print-certs "$f" 2>/dev/null | grep -o "Signer #1 certificate SHA-256 digest: [0-9a-f]*" | awk '{print $NF}')
  python3 - "$mine" "$id" "$vc" "$vn" "$lab" "$ms" "$sg" "$(basename $f)" <<'PY'
import json,sys
m=json.loads(sys.argv[1]); exp=dict(id=sys.argv[2],versionCode=int(sys.argv[3] or 0),versionName=sys.argv[4],label=sys.argv[5],minSdk=int(sys.argv[6] or 0),signer=sys.argv[7])
bad={k:(m.get(k),v) for k,v in exp.items() if m.get(k)!=v}
print(("OK  " if not bad else "BAD ")+sys.argv[8], "" if not bad else bad, "icon="+str(m.get('icon')))
PY
done
