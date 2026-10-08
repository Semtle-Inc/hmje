# 소스 출처

모든 소스는 아래 고정 커밋의 Git archive에서 내보냈습니다. 작업 트리와 원본 Git 이력은 포함하지 않습니다.

| 구성 | 원본 프로젝트 | 정확한 커밋 |
| --- | --- | --- |
| compiler / library / platforms | hmje-toolchain | `032483bd83c275837d0a277db125f651bf34b678` |
| libraries/hmje-llm | hmje-llm | `796fdc10c92342070268eb5075d2228b6f960871` |
| libraries/hmje-sem | hmje-sem | `9a0986e66342242d57469b17b17428f58075f820` |
| libraries/hmje-realtime | hmje-realtime | `3ae56612ec60c49697e0ecf2112e29867abe53e8` |
| libraries/hmje-network | hmje-network | `5257cb8d0843e213906cfbab0059427201a04786` |
| libraries/hmje-ui | hmje-ui | `f86d7421e77349f4c053d25a836862dde89c1358` |
| libraries/hmje-crypto | hmje-crypto | `e22f5a33c01cc528918125dcb5a7138c3470abca` |
| libraries/hmje-vault | hmje-vault | `d0a9cc3ace78ffbb1d83c50c29b1e0fa10369b19` |
| libraries/hmje-jindo | hmje-jindo | `18f7d3c90bd702316aee753ac197c32fc0e40bdd` |
| libraries/hmje-host | semtle-ide/libs/hmje-host | `43400f8b176c3a630c93b55dedb8b8a75823de4f` |
| libraries/hmje-syntax | semtle-ide/libs/hmje-syntax | `43400f8b176c3a630c93b55dedb8b8a75823de4f` |

공개 트리에서는 상대 의존 경로·문서 위치·공개 진단 안내를 조정하고, 이전 도구와 내부 문서·실험 자료를 제외했습니다. 원본 저장소의 이력과 작업 트리 파일은 포함하지 않았습니다.

Beta 1 빌드 `2026100804`의 패키지 툴체인 커밋은 `d2ce1c947dcc32ea40fd242a530e36bbf22a1790`입니다. 컴파일러·런타임·플랫폼·드라이버 소스는 위 `032483bd` 기준과 동일하며, 패키지 커밋은 HmjeUI 고정 버전을 갱신합니다. HmjeUI의 도형 검사 HMJE 소스는 보존하고 실행기는 POSIX 셸로 제공합니다.

## 공개 문서 구성

라이브러리 README와 API 안내는 공개 트리의 모듈·플랫폼 지원·문서 경로를 기준으로 편집했습니다. 개발 이력·비공개 로드맵·제외된 검사 명령은 사용자 안내에서 제거했습니다. Swift 씨앗 재현 검증은 별도 `hmje-swift-seed` 저장소에 있으며 [부트스트랩 안내](docs/bootstrap.md)에서 연결 절차를 설명합니다. 위 소스 커밋과 라이브러리 고정 버전은 변경하지 않습니다.
