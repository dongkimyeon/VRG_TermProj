# 전선·냉각수 배관 모델

2026-09-22. Blender 5.2.2 LTS에서 제작한 경로 편집형 아트 에셋이다.
참고 이미지의 Patch Manager처럼 전선과 배관을 개별 Bezier 커브로 유지한다.
이번 요청의 ‘패치 기반’은 경로를 편집하는 방식으로 해석했다.

## 파일

- `VRG_Connections.blend`: 편집 원본. `VRG_Infrastructure` 장면을 사용한다.
  작업 전 기본 장면은 `Scene`으로 보존했다.
- `VRG_Infrastructure.fbx`: 전선·배관·피팅·트레이·CDU의 메시 교환 파일.
  랙 본체와 전시 바닥은 제외했다.
- `VRG_Connections.glb`: 랙과 바닥을 포함한 전체 배치 확인용 메시 파일.
- `overview.png`, `rack_detail.png`, `pipe_detail.png`: 실제 Blender 렌더.
- `routes_manifest.json`: 역할, 지름(m), 연결 지점 이름, 원래 경유점과
  실제 Bezier 제어점/핸들(m). UE 물리 노드 ID와 연결된 데이터는 아니다.
- `verification.json`: FBX 재가져오기 객체 수·경계 좌표·단위 검증과 메시 삼각형 수.
- `build_infrastructure.py`: 모델 재생성 스크립트.
- `export_and_preview.py`: 렌더·메시 내보내기·FBX 재가져오기 검증 스크립트.

## 참고 자료와 적용한 형태

프로젝트의 `CLAUDE.md`, `남은작업.md`, `구현순서.md`와 `Image/`의 11개 이미지를 확인했다.

| 이미지 파일 뒤쪽 시각 | 적용 내용 |
|---|---|
| `190547` | 상부 트렁크에서 랙별로 내려오는 다발 배선, 패치 연결 |
| `190658`, `190713`, `190731` | 사다리형 트레이, 둥근 코너, 행 사이 상부 연결 |
| `190725` | 랙 뒤 세로 공급·회수 매니폴드와 반복 연결부 |
| `190738` | 청록·연두·주황 데이터 배선, 검정 전원 배선 |
| `191036` | 바닥 공급/회수 헤더, CDU, 분기와 지지대 |
| `190927`, `190948`, `190955`, `191025` | 검정 프레임·금속 트레이·포트 형태의 랙 문맥 |

사진의 인상과 연결 구조를 참고한 모델이며, 제조사의 정확한 CAD 복제는 아니다.
18개 컴퓨트 트레이를 가진 랙 4대씩 2줄, 총 8대로 구성했다.
두 행의 냉각 배관은 각각의 CDU에 연결된다.

## 단위와 크기

Blender 1 unit = 1 m, Metric, Unit Scale 1.0이다.
`DataCenterRoomActor.h`의 시각화 기본값을 가져왔다.

| 항목 | 크기 |
|---|---|
| 랙 | 폭 0.6 × 깊이 1.2 × 높이 2.0 m |
| 행 방향 랙 피치 | 0.66 m |
| 마주 보는 랙 앞면 사이 | 2.4 m |
| 바닥 헤더 중심 높이 | 0.25 m |
| 헤더 지름 / 분기관 지름 | 0.11 m / 0.05 m |
| 공급/회수 헤더 간격 | 0.16 m |
| 상부 케이블 트레이 높이 | 2.85 m |

작은 호스(지름 13 mm), 데이터선(4.8~16 mm), 전원선(11 mm), 커넥터는
참고 화면의 비율에 맞춘 **아트 치수**이며 실제 장비의 사양이나 계산 입력이 아니다.
모델의 X는 랙 행 방향/폭, Y는 랙 깊이, Z는 높이다.
UE의 `RackSizeM`는 X=깊이/Y=폭이므로 연결 시 좌표 매핑과 회전을 명시해야 한다.
FBX에는 미터→센티미터 단위 정보를 내보냈으며 Blender 재가져오기로 검증했다.

## Blender에서 편집

Outliner 컬렉션을 기준으로 원하는 계통을 선택한다.

| 컬렉션 | 내용 |
|---|---|
| `VRG_01_RackContext` | 랙·트레이·CDU 본체 |
| `VRG_02_ColdSupply` | 파랑 공급 헤더·라이저·호스 |
| `VRG_03_WarmReturn` | 빨강 회수 헤더·라이저·호스 |
| `VRG_04_DataCables` | 청록 상부 트렁크·드롭, 연두/주황 패치선 |
| `VRG_05_PowerCables` | 검정 전원선 |
| `VRG_06_TraysSupports` | 데이터/전원 트레이·지지대·케이블 정리부 |
| `VRG_07_Fittings` | 커플링·밸브 손잡이·플러그 |
| `VRG_08_Presentation` | 바닥·카메라·조명·범례 |

예: `A01_SUPPLY_Riser`를 선택하고 Tab으로 Edit Mode에 들어가 제어점을 이동한다.
Curve Data → Geometry → Bevel Depth는 반지름이다.
호스와 피팅은 별도 오브젝트이므로 경로를 옮기면 연결 피팅도 함께 옮겨야 한다.
랙을 이동했을 때 자동 재배선하거나 피팅을 따라 움직이는 기능은 구현하지 않았다.
수정한 커브를 재내보낼 때는 `validate()`로 manifest도 갱신한다.
`bezier_knots_m`이 현재 커브 형상이고 `points_m`은 생성 당시 경유점이다.

카메라는 `CAM_Overview`, `CAM_Rack_Detail`, `CAM_Pipe_Detail` 세 개다.
바닥을 숨기고 싶으면 `VRG_08_Presentation`을 숨기되 렌더 카메라와 조명도
같은 컬렉션에 있다는 점에 유의한다.

## 재생성

Blender Scripting에서 `build_infrastructure.py`를 열고 실행하거나 다음과 같이 실행한다.

```powershell
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' --background --python '<repo>/Art/Infrastructure/build_infrastructure.py'
& 'C:/Program Files/Blender Foundation/Blender 5.2/blender.exe' --background '<repo>/Art/Infrastructure/VRG_Connections.blend' --python '<repo>/Art/Infrastructure/export_and_preview.py'
```

`<repo>`를 저장소 절대 경로로 바꾼다. 재생성은 새 Blender 세션에서 한다.
같은 이름의 장면이 이미 있으면 스크립트는 중단하며 기존 장면을 지우지 않는다.
재생성/내보내기는 위 출력 파일들을 갱신하므로 수작업 수정본은 다른 이름으로 저장한다.
교환 파일에서는 커브 분할 수를 낮추고 메시로 평가한다. 원본 .blend의 커브는 유지된다.

## 검증과 경계

Blender MCP로 현재 장면의 오브젝트와 뷰포트를 확인하고 세 카메라의 렌더를 점검한다.
내보내기 스크립트는 별도 임시 장면에서 FBX를 다시 가져와 객체 수와 실제 월드 경계(m)를
대조한다. 결과 숫자는 `verification.json`에 기록된다.

- 색은 **공급/회수/배선 종류**의 구분이다. 계산된 온도·유량·전력 표시가 아니다.
- 배관은 외형용 표면이다. 내부 유체 체적·관벽 두께·압력 손실을 모델링하지 않았다.
- 완전한 배관 간섭 검사나 제조용 최소 굽힘 반경 검증은 하지 않았다.
- UE 스플라인 액터·수력 그래프와의 연결, 랙 이동 시 재라우팅은 후속 작업이다.
  FBX를 가져와도 Unreal SplineComponent가 자동 생성되지 않는다.
- UE에서 사용 시 기존 솔버의 연결 관계로 경로를 결정해야 한다.
  이 예시 배치의 연결 지점 이름을 물리 노드 ID로 간주하면 안 된다.
- UV 텍스처 저작·LOD·충돌 메시·라이트맵 UV는 추가 제작 대상이다.
  재질은 단색 PBR이다. UE 재질 변환과 실제 VR 성능은 측정하지 않았다.
- C++·레벨·솔버를 수정하지 않았다. UE 빌드/Automation Test는 실행하지 않았고,
  S5/S6/S7/S2b 물리 단계 완료를 의미하지 않는다.
