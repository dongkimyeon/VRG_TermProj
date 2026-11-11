# Web/vendor — 웹 대시보드 3D 보기용 three.js 사본

인터넷 없이(오프라인·패키지) 3D 방 보기를 쓰려고 넣은 외부 라이브러리 사본이다(2026-11-11, 사용자 승인 뒤 내려받음).
게임이 `GET /vendor/three.min.js`, `GET /vendor/OrbitControls.js`로 내주고, 페이지는 이 사본을 먼저 읽고 없으면 CDN을 읽는다.
`VRG_TermProj.Build.cs`의 `RuntimeDependencies`가 `Web/` 전체를 패키지에 같은 상대 경로로 넣는다.

| 파일 | 출처 | 크기 | SHA-256 |
|---|---|---|---|
| `three.min.js` | https://cdn.jsdelivr.net/npm/three@0.128.0/build/three.min.js (REVISION "128") | 603,445 B | `9274BBCEC8D96168626C732B5D31C775AA8CFB7EAA0599BEC0C175908A2C1CE2` |
| `OrbitControls.js` | https://cdn.jsdelivr.net/npm/three@0.128.0/examples/js/controls/OrbitControls.js | 26,375 B | `02BB4ADE710F3E607329E37A21F098BC3AC70EB6E33DAF8A65E79F4DB785E7B2` |

라이선스: MIT, Copyright 2010-2021 Three.js Authors (`three.min.js` 머리 주석 그대로 둔다). 고치지 않는다. 버전을 바꾸면 페이지의 CDN 주소(`Web/index.html`)도 같이 바꾼다.
