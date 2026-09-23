# GB200 랙용 파이프·케이블 확인용 키트

2026-09-23. 프로젝트 저장소 밖에서 제작했으며 커밋하지 않았다.
기존 랙 틀·컴퓨트 노드·사진형 Quantum 스위치는 그대로 사용했다.
진영님 커밋 `5ec749b`의 `Art/Infrastructure`에서 색상, 매니폴드, 패치 배선,
사다리형 트레이와 둥근 경로 생성 방식을 참고했다.

## 먼저 열어볼 파일

- `GB200_Connections.blend`: Blender 원본. `01_Connected_Racks`에 랙 2대 연결 예시,
  `02_Modular_Parts`에 별도 부품이 있다. 기존 모델도 파일에 포함되어 있다.
- `previews/01_front_connections.png`: 전면(흡기면). 2026-09-23부터 데이터 배선은 전면에 없다.
- **데이터는 후면이다 (2026-09-23).** 노드 IO·스위치 포트·노드–스위치 패치·랙 사이 링크를 냉각수와 같은
  후면(−X)으로 옮겼다. 랙은 돌리지 않았고 스위치만 요 180°로 역방향 장착한다. 좌표는
  `scripts/rear_data_layout.py`, 기존 파일 수정은 `scripts/migrate_rear_data.py`(Blender 5.2),
  배치 표는 저장소 `Tools/GJ_모델_적용.md`의 "포트 배치" 절에 있다.
- `previews/04_switch_patches.png`: 후면 스위치 포트 확대. `UE_GJ_Front.png` / `UE_GJ_Rear.png`는 UE L_Test 화면.
- `previews/02_rear_cooling.png`: 공급·회수 매니폴드와 CDU 연결.
- `previews/03_hose_connectors.png`: 냉각 연결부 확대.
- `UnrealSample/ConnectionsPreview.uproject`: 본 프로젝트와 분리한 UE 5.8.2 확인용 프로젝트.
  `/Game/L_ConnectionsPreview` 맵을 연다. 기본 모델과 연결 메시를 이 프로젝트에만 가져온다.

## 만든 부품

| 구분 | 내용 |
|---|---|
| 변형 가능한 선 본체 | 데이터 4.8 mm, 전원 11 mm, 냉각 호스 13 mm, 라이저 50 mm, 헤더 110 mm |
| 단단한 연결부 | 노드용/스위치용 플러그, 공급/회수 퀵 커넥터, 매니폴드 티, 차단 밸브, CDU 어댑터 |
| 예시 배치 | 노드 냉각 호스 72개, 노드–스위치 패치 36개, 랙 사이 링크 4개, 라이저 4개·공급 분기 2개, CDU 헤더 2개 |

총 12개 모듈을 개별 FBX와 GLB로 내보냈다. 본체와 피팅은 별개여서
호스를 늘여도 플러그·밸브가 함께 늘어나지 않는다. 검정 전원선은 재사용할 수 있는 본체만
제공한다. 기존 모델에 확정된 전원 포트/PDU가 없으므로 임의의 전원 연결을 붙이지 않았다.

크기는 **현재 만든 모델에 맞춘 아트 치수**다. 공급사 인증 피팅 규격이나 제조용 CAD가 아니다.
특히 스위치는 사진 외형을 우선한 기존 추정 모델에 맞춘 것으로, 실제 양산 제품 규격으로
해석하지 않는다. 파랑/빨강/연두/청록은 역할 색이며 계산된 온도·유량이 아니다.

## Blender에서 수정

Outliner의 Supply / Return / Data_Patches 컬렉션에서 선을 선택하고 Tab으로 들어가
Bezier 제어점을 이동한다. Curve의 Bevel Depth는 선 반지름이다.
커넥터는 별도 컬렉션 인스턴스다. 개별 수정은 해당 원본 컬렉션 안의 메시를 편집한다.
Blender 배치 예시에는 랙 이동 시 자동 재배선 기능이 없다.

## 언리얼에서 늘이기·줄이기·휘기

FBX 자체에는 스플라인 편집 기능이 없으므로 별도의 `GB200PatchCable` C++ 액터를 넣었다.
코드는 `UnrealSample/Plugins/GB200Connections/Source/`에 있다.

1. 확인용 맵에서 `AUTO_PATCH_Move_Endpoint_Actors`를 찾는다.
2. `MOVE_ME_Start_Port` 또는 `MOVE_ME_End_Port`를 옮긴다. Auto Path 모드는 에디터 틱에서
   명시적으로 지정된 끝점을 따라 경로를 다시 만든다. `Rebuild Connection`으로 수동 갱신도 가능하다.
3. 선 액터의 `Sag Cm`은 시각적인 늘어짐, `Lead Cm`은 커넥터 앞 직선 여유를 조절한다.
4. 직접 모양을 잡으려면 `Auto Path`를 끄고 `EditablePath`의 제어점과 접선을 편집한다.
   필요하면 `Rebuild Connection`을 누른다. 이미 배치된 120개 예시 경로는 이 수동 모드다.
5. `Start/End Anchor`에는 랙 액터도 지정할 수 있다. `Offset Cm`은 해당 액터의 로컬 포트 위치,
   `Outward`는 포트가 향한 로컬 방향이다. 시작/끝 플러그 메시와 길이는 각각 지정한다.

선 본체는 X=0~100 cm, 길이 방향 50분할이며 `SplineMeshComponent`의 Forward Axis는 X다.
몸통은 원형 단면을 유지하고 경로를 따라 길이만 변한다. 연결부 충돌은 꺼져 있다.
불연속 Constant 점·닫힌 루프 대신 Curve/Linear의 열린 경로를 사용한다.
에디터 사용 시 액터 스케일은 1을 유지하고 경로 점으로 길이를 바꾼다.

## 패치 자동 연결 / CDU 연동의 경계

현재 구현한 것은 **명시한 두 끝점 사이의 시각적 연결과 편집**이다.
랙을 발견해 빈 포트를 선택하는 Patch Manager, 장애물 회피, 배선 충돌 검사,
포트 중복 점유 검사, 실제 중력·장력·케이블 충돌 시뮬레이션은 구현하지 않았다.
Sag는 중력 솔버의 결과가 아닌 모양 조절값이다. 배관은 물성상 신축한다는 뜻이 아니라
배치 설계 시 길이·경로를 다시 지정한다는 의미다.

다음 통합 때는 기존 `DataCenterRoomActor`의 수력 그래프를 연결 관계의 기준으로 사용한다.
예시 곡선 이름이나 포트 ID로 별도 물리 그래프를 만들지 않는다.
CDU 연결점은 `GetSupplyPortWorldM()` / `GetReturnPortWorldM()` 반환값에 100을 곱해
UE 월드 좌표로 넘긴다. 예시 CDU는 현재 코드 기본값인 크기 0.9×0.8×1.9 m,
로컬 공급 (+0.45,-0.15,0.25) m / 회수 (+0.45,+0.15,0.25) m에 맞춘 프록시다.
어댑터는 시각화의 120 mm 포트와 110 mm 헤더를 잇는다.
실제 프로젝트 액터·수력 솔버·레벨은 변경하지 않았다.

## 단위·재생성·AI 참고 파일

- Blender: m, +X 전면, +Z 위. UE FBX: cm, Import Scale=1, Force Front X Axis=false.
  Blender 경로를 UE로 바꿀 때 `(x,y,z) -> (100x,-100y,100z)`를 사용한다.
- `reports/connection_manifest.json`: 부품 치수, 재질, 포트 위치·방향·역할, Bezier 핸들.
  포트 좌표는 예시 장면 월드 좌표이며, 임의 배치의 로컬 포트 좌표가 아니다.
- `scripts/build_connections.py`: Blender 재생성. 기존 모델은 옆 폴더
  `GB200_NVL72_Models_20260922/GB200_Modular_Kit.blend`에서 읽는다.
- `scripts/unreal_preview.py`: 확인용 UE 프로젝트에만 가져오기·예시 생성·검증.
  같은 이름의 확인용 맵/에셋을 갱신하므로 직접 수정한 맵은 다른 이름으로 저장한다.
- `scripts/validate_fbx.py`: FBX 재가져오기 치수·유효 메시·UV 검사.
- `reports/`: 실제 실행 로그와 검증 결과. 성공 여부는 마지막 결과 파일을 확인한다.

재생성 명령:

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' --background --threads 6 --python '<이 폴더>/scripts/build_connections.py'
& 'E:/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<이 폴더>/UnrealSample/ConnectionsPreview.uproject' -run=pythonscript '-script=<이 폴더>/scripts/unreal_preview.py' -unattended -NullRHI
```

플러그인 수정 빌드는 한글 경로에서 MSVC PCH 생성 오류가 발생해
`C:/Users/Public/Documents/GB200ConnectionBuild_20260923`에서 수행했다.
확인용 프로젝트에는 그 빌드의 Win64 Editor 바이너리를 복사했다.
다른 엔진 버전에서는 소스로 다시 빌드해야 한다. 패키징/VR 성능은 검증 대상에 포함하지 않았다.

## 이번에 실행한 검증 결과

- Blender 렌더 4장을 확인했다. FBX 12종 재가져오기에서 치수·UV·유효 삼각형 검사를 통과했다.
- 커넥터 꼬리와 선의 중심선 끝점 226곳을 대조했다. 최대 차이는 약 0.000060 mm다.
  이 수치는 좌표 정합 오차이며 실물 제작 공차나 전체 간섭 검사 결과가 아니다.
- UE 5.8.2 Win64 Development Editor용 플러그인 빌드 성공.
- NullRHI에서 12종 가져오기/재가져오기와 경로 생성 검사 성공. 최종 로그 경고 0, 오류 0.
- 끝점 변경 후 재구축: 길이 168.88 → 240.64 cm로 늘어남, 107.12 cm로 줄어듦.
  Sag=30 cm에서 중간점 Z=85 cm를 확인했다. 플러그 연결 끝점 오차는 0.000001 cm 미만이다.
- 저장 후 맵 재열기: 수동 경로 120개와 자동 패치 예시 1개 모두 메시가 다시 생성됐다.
- 실제 에디터 마우스 조작·렌더 프레임률은 검사하지 않았다. 자동 따라오기 검증은
  끝점을 바꾼 뒤 명시적으로 재구축한 결과이며, 에디터 틱 경로의 실시간 조작 확인은 남아 있다.
- 프로젝트 물리 Automation Test는 실행하지 않았다. 이 작업은 S5/S6/S7 물리 단계 완료가 아니다.

