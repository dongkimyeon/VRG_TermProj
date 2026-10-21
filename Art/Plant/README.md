# 설비 마당 외형 모델 (2026-10-21)

`모델링_요청.md`의 엔진 기본 도형을 대체하는 외형 에셋이다. 기존 랙·스파인·CDU·CRAH는 유지한다.
장비마다 편집 부품 컬렉션, 단일 메시 FBX/GLB, UV0, 단색 PBR 재질과 미리보기를 제공한다.

| 장비 | 구현한 외형 | 삼각형 / 상한 |
|---|---|---|
| DryCooler | 팬 2개·날개·원형 가드, 양쪽 핀 코일, 프레임·다리, 물 접속 플랜지 | 7,056 / 8,000 |
| Chiller | 원통 열교환기 2개, 압축기·분리기, 볼트 덮개·밴드, 제어반·HMI | 7,096 / 15,000 |
| Pump | 모터 방열핀·팬 덮개, 커플링, 비대칭 볼류트, 흡입/토출 플랜지 | 2,992 / 5,000 |
| PlateHx | 판 30장, 두 압력 프레임, 가이드 바·체결봉·너트, 노즐 4개 | 4,768 / 8,000 |
| Valve2Way | 플랜지 밸브 몸체, 스템, 전동 액추에이터 | 1,388 / 3,000 |
| CheckValve | 플랜지 몸체와 상부 점검 덮개 | 1,240 / 2,000 |
| LeakMarker | 얇고 불규칙한 물웅덩이 표시 | 156 / 1,000 |

크기는 요청서에 정한 **프로젝트 도식 치수**다. 특정 제품의 정격·실측 크기나 제조사 CAD를 사용하지 않았다.
로고·글자·사진 텍스처·새 물성 값은 없다. 내부 유로, 움직이는 팬과 동작 HMI, 자동 LOD는 포함하지 않는다.

## 파일과 사용

- `Plant_Reference.blend`: `Plant_DryCooler` 등 7개 편집 컬렉션. 기본으로 드라이쿨러만 보인다.
  Outliner에서 원하는 장비 컬렉션의 뷰포트/렌더 표시를 켜고 다른 장비를 끈다. `Studio`는 조명·카메라·바닥이다.
- `build_plant.py`: 원본·모든 교환 파일·검사 JSON·7장 렌더를 재생성. Blender 5.2에서 실행한다.
- `SM_Plant_*.fbx`: 정점을 cm로 내보낸 UE 에셋. `SM_Plant_*.glb`와 Blender 원본은 m.
- `plant_*_preview.png`: Blender에서 만든 미리보기. 언리얼 캡처와 구분한다.
- `materials.json`, `verification.json`: PBR 값, 실제 경계·원점·UV·삼각형 수와 노즐 좌표.
- `ue_import_verification.json`: UE 가져오기 후 크기·재질 검사. 실행 결과를 기록한다.
- `ue_plant_*.png`, `ue_screen_verification.json`: L_Test의 실제 설비 마당 화면과 그때 그려진 모델 개수.

모든 장비는 원점이 바닥 중심, +X가 전면이다. 각 명목 상자 안에 플랜지·그릴까지 넣었다.
Blender 경계 최대 오차는 1e-6 m 미만으로 검사한다. 구체적인 값은 `verification.json`을 본다.

## 언리얼 연결

`Tools/install_plant_model.py`는 `/Game/Connections/SM_Plant_*`와 `Materials/MC_Plant_*`만 가져온다.
레벨을 저장하지 않는다. 이 경로는 기존 AlwaysCook 설정 대상이다.
`PlantVisual::AppendEquipment`를 배치 액터와 자동 설비 마당이 함께 사용한다.
수십 대의 드라이쿨러는 메시·강조색별 ISM으로 묶는다. 서로 다른 메시의 그룹 키를 분리해 장비가 섞이지 않도록 한다.
운전/정지/고장 색은 **MC_Plant_Accent** 슬롯에만 적용하며 금속·코일·제어반은 원래 PBR을 유지한다.
에셋이 없으면 기존 부품 도형으로 표시하고 `vrg.Plant.ReferenceModels 0`으로도 비교할 수 있다.
배치 액터는 Details의 `Refresh Visuals`로 바로 갱신한다.

**접속구 선택:** 요청서 끝의 허용 대안에 따라 기존 `PlantVisual::PortLocalM`를 보존했다.
드라이쿨러는 −Y 옆면의 x=±0.85, z=0.65 m, 칠러는 ±Y 옆면의 x=−0.75/+0.60, z=0.55 m에 연결한다.
펌프는 기존 +X 흡입/상부 토출 위치를 사용한다. 판형 열교환기는 요청서의 +X 노즐 4개를 표현한다.
판형 열교환기의 별도 노즐 배선은 추가하지 않았다. 기존 수측 이코노마이저의 계산 연결을 따른다.
배관·케이블·열/수력 솔버·장비 대수 결정·선택/충돌 상자·배치 저장은 바꾸지 않았다.
누수 표시는 계산된 누수 노드의 XY를 바닥에 투영한 얇은 표시이며 물 확산 해석이 아니다.

## 재생성

프로젝트 루트에서 실행한다. 실행 파일과 `<repo>`는 각 PC 경로로 바꾼다.
생성 명령은 출력물을 갱신하므로 수작업 편집본은 다른 이름으로 보관한다.

```powershell
& '<Blender>/blender.exe' -b --python-exit-code 1 --python Art/Plant/build_plant.py
& '<UE>/Engine/Build/BatchFiles/Build.bat' VRG_TermProjEditor Win64 Development '-Project=<repo>/VRG_TermProj.uproject' -WaitMutex -MaxParallelActions=2
& '<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' -run=pythonscript '-script=<repo>/Tools/install_plant_model.py' '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI
& '<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' '-ExecCmds=Automation RunTests VRG.Plant' '-TestExit=Automation Test Queue Empty' -unattended -NullRHI '-ReportExportPath=<repo>/Saved/Automation/PlantArt'
```

생성 시 극소 퇴화 면을 정리하고 법선을 다시 계산해 삼각형화한다.

## 외형 참고 출처 (제품 치수·성능은 사용하지 않음)

- [Güntner Flat COMPACT](https://guntner.com/products/dry-coolers/flat-compact-dc): 수평 팬과 측면 코일의 외형.
- [Trane 수냉식 칠러](https://www.trane.com/commercial/north-america/us/en/products-systems/chillers/water-cooled-chillers.html): 셸·압축기·제어반 구성.
- [KSB Etanorm](https://www.ksb.com/en-ca/lc/products/pump/dry-installed-pump/etanorm-etanorm-myflow-etanorm-pro/e04b): 모터와 단흡입 펌프 외형.
- [Alfa Laval 판형 열교환기](https://www.alfalaval.com/microsites/gphe/): 판 묶음·압력판·체결 볼트 구성.

## 검증 범위

실행한 검사와 스크린샷 결과는 `docs/기록/07_설비냉각.md`의 이 작업 기록을 따른다.
물리 완료 단계의 승격, 전체 패키징, 실제 장비 치수 보정, 최악 배치의 FPS 검증을 의미하지 않는다.


### 실행·확인 기록 (2026-10-21)

- **제작/구현:** 장비 7종 제작, 배치 액터와 자동 설비 마당의 메시 교체, 강조 재질만 상태색 적용 완료.
- **Blender 검사:** 명목 경계·바닥 중심 원점·전면 축·단일 메시·UV0 1개·재질 수·삼각형 상한 통과.
  최대 경계 오차 7.15256e-08 m(요청서 기준 1e-6 m 미만).
- **UE 실행 검사:** UE 5.8.2 Editor 최종 빌드 성공. 7종 가져오기 후 크기/재질 검사 PASS,
  Blender 경계 대비 최대 차이 1.28746e-07 m. 별도의 UE UV/충돌 개수 재조회 검사는 실행하지 않았다.
- **자동화:** `VRG.Plant` 38개 성공 / 실패 0 / 미실행 0
  (무경고 성공 36개 + 경고 동반 성공 2개).
  두 경고는 엔진의 외부 연결 확인(`generate_204`) HTTP 시간 초과다.
  새 `ReferenceModelsAndAccent`는 7개 메시 동시 표시·그룹 분리·재질 유지·상태색·기본 도형 대체를 확인한다.
- **사진:** L_Test SIE의 실제 자동 설비 마당에서 전체/장비별 근접과 설비 켠 화면을 저장했다.
  드라이쿨러 32대, 칠러 5대, 펌프 9대, 판형 열교환기 1대, 제어 밸브 15개, 체크밸브 9개가 그려졌다.
  레벨은 저장하지 않았다. `ue_screen_verification.json`은 캡처 당시의 모델 수/파일 기록이다.
- **사용자 확인:** 2026-10-21 사용자가 "사진 확인함"이라고 확인했다. 사진 확인 범위로 기록하며,
  배치 입력·패치 맵·온도/유량 판독·FPS·전체 기능의 직접 확인 완료로 확대하지 않는다.
- **남은 범위:** 기존 설비 배관 표시 겹침 진단 6쌍을 L_Test에서 관측했다. 배관 경로는 이번에 수정하지 않았다.
  LOD·내부 장비·팬/HMI 동작·실측 치수 보정·FPS·패키징·배치 편집 전체 확인은 별도다.

이번 PC는 i5-8500/RX 580/16 GB다. 빌드 정체 후 `-NoUBA -nocache -UBADisableWaitOnMem`으로 완료했다.
캡처는 초기 콘솔 요청의 저장 지연 이후 에디터 뷰포트 캡처 API와 `-RenderOffscreen`을 사용했다.
이 문서 정리에서는 빌드·테스트·캡처를 다시 실행하지 않았다.
