# 소스 출처

구성요소별 원본 소스 버전은 다음과 같습니다. 소스는 각 커밋의 Git archive를 기준으로 하며, 원본 저장소의 Git 이력은 포함하지 않습니다.

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

## 배포 소스 구성

이 저장소에는 컴파일러·런타임·플랫폼 연결과 공개 라이브러리 10개를 포함합니다. IDE 본체, 비공개 구성요소, 이전 빌드 도구와 실험 자료는 포함하지 않습니다. 의존성 경로와 문서 링크는 이 저장소의 디렉터리 구성을 사용하며, 컴파일러의 안내 진단은 공개 라이브러리 범위를 기준으로 합니다.

Beta 1 빌드 `2026100804`의 패키지 툴체인 커밋은 `d2ce1c947dcc32ea40fd242a530e36bbf22a1790`입니다. 컴파일러·런타임·플랫폼·드라이버 소스는 위 `032483bd` 기준과 동일하며, 패키지 커밋은 HmjeUI 고정 버전을 갱신합니다. HmjeUI의 도형 검사 HMJE 소스는 보존하고 실행기는 POSIX 셸로 제공합니다.

## 문서 및 검증

라이브러리의 모듈·플랫폼 지원과 API는 [라이브러리 안내](libraries/README.md), 소스 빌드의 입력과 결과는 [검증 결과](docs/publication-status.md)에서 확인할 수 있습니다. Swift에서 시작하는 컴파일러 구축 및 재현 검증은 [hmje-swift-seed](https://github.com/Semtle-Inc/hmje-swift-seed)에서 제공합니다. 두 저장소를 사용하는 절차는 [부트스트랩 안내](docs/bootstrap.md)에 있습니다.
