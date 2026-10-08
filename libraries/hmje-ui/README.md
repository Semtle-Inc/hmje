# hmje-ui — 선언형 화면과 그래픽

화면 선언, 상태 추적, 레이아웃, 텍스트·이미지·입력·제스처와 플랫폼 그래픽 연결을 제공합니다. HmjeUI와 저수준 HmjeGraphics 모듈을 내보냅니다.

## 사용

HMJE 1.0.0-beta.1 툴체인의 SDK에 포함됩니다. 소비 프로젝트의 모듈에 사용할 라이브러리를 의존성으로 선언하고, HMJE 소스에서 모듈을 가져옵니다. 내보내는 모듈은 HmjeUI와 HmjeGraphics입니다.

```hmjep
모듈 App의
    원본 경로는 "Sources/App"이고,
    의존 라이브러리는 HmjeUI이다.
```

```hmje
HmjeUI를 가져온다.
```

프로젝트·산출물의 전체 구성은 [프로젝트 명세](../../docs/project.md), 이 트리에서 SDK를 빌드·설치하는 절차는 [소스 빌드](../../docs/building.md)를 참고하십시오.

## 기능

| 구분 | 제공 내용 |
| --- | --- |
| 화면과 상태 | 화면 구성·추적 대상·화면 기억·비동기 작업 |
| 표시와 입력 | 글꼴·이미지·테마·스크롤·텍스트 입력·제스처 |
| 데스크톱 | 창·파일 선택·파일 감시·알림 |
| 그래픽 | 그리기 명령·GPU 백엔드·표면 표시 |

## 지원 범위와 사용 조건

대상별 창·입력·그래픽 기능의 범위가 다릅니다. 데스크톱 창 API는 macOS·Windows·Linux에 연결되며 모바일 대상에서는 해당 기능의 미지원 진단을 제공합니다. 기본 요소와 테마를 포함하며 앱의 디자인 시스템은 공개 테마 API로 구성합니다. 호스트 GPU 명령의 실패 정책은 렌더링 문서를 참고합니다.

## 문서와 소스

- [모듈·플랫폼별 빌드 구성](Project.hmjep)
- [구현 소스](Sources/)
- [테마](docs/theme.md)
- [상태 추적](docs/tracked-classes.md)
- [GPU 렌더링](docs/gpu_renderer.md)
- [비동기 이미지](docs/async_image.md)
- [입력 보안](docs/secure-input.md)
- [데스크톱 파일 선택](docs/desktop-file-dialog.md)
- [파일 감시](docs/file-watch.md)
- [알림](docs/notifications.md)
- [가로 스크롤](docs/horizontal-scroll.md)
- [선택 목록](docs/select-overlay.md)
- [클립보드](docs/clipboard.md)
- [색 토큰](docs/color-tokens.md)
- [Enter 동작](docs/enter-action.md)
- [변환 제스처](docs/transform-gesture.md)

## 버전과 라이선스

Beta 1의 API는 안정화 중입니다. 고정된 원본 커밋은 [소스 출처](../../SOURCES.md), 공통 변경 사항은 [릴리스 노트](../../RELEASE_NOTES-1.0.0-beta.1.md)를 확인하십시오. 앱에 포함하는 배포에는 [런타임·라이브러리 라이선스](../../LICENSE-RUNTIME)가 적용됩니다.
