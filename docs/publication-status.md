# 배포 구성 및 검증 결과

HMJE **1.0.0-beta.1 · 빌드 `2026100804`**는 2026-10-09에 공개되었습니다.

## 배포 구성

| 자료 | 공개 위치 | 포함 내용 |
| --- | --- | --- |
| 소스 및 문서 | [hmje](https://github.com/Semtle-Inc/hmje) | 컴파일러, 런타임, 표준·공개 라이브러리, 플랫폼 연결, 문서, 예제 및 빌드 도구 |
| 개발자 세션 | [hmje-developer-sessions](https://github.com/Semtle-Inc/hmje-developer-sessions) | S1–S6 예제, 학습 말뭉치, 빌드·검증 도구 및 검증 보고서 |
| Swift 씨앗 | [hmje-swift-seed](https://github.com/Semtle-Inc/hmje-swift-seed) | Swift 초기 컴파일러, 부트스트랩 소스 아카이브·패치, 고정 버전·해시 및 재현 검증 보고서 |
| 설치 패키지 | [Beta 1 릴리스](https://github.com/Semtle-Inc/hmje/releases/tag/v1.0.0-beta.1) | macOS·Windows·Linux의 ARM64 및 x86_64용 IDE·툴체인 |

운영체제별 다운로드와 개발 도구 요구 사항은 [설치 안내](../README.md#설치)에 있습니다. 다운로드 파일의 해시는 릴리스의 [SHA256SUMS](https://github.com/Semtle-Inc/hmje/releases/download/v1.0.0-beta.1/SHA256SUMS)로 확인합니다.

IDE 본체와 비공개 구성요소의 소스, 인증서·프로비저닝 프로파일은 포함하지 않습니다. 소스 저장소에는 생성된 앱·실행 파일·로그·모델·체크포인트를 포함하지 않습니다.

## 소스 버전

| 구성 | 원본 버전 |
| --- | --- |
| 컴파일러·런타임·플랫폼·드라이버 | `032483bd83c275837d0a277db125f651bf34b678` |
| 배포 패키지의 툴체인 | `d2ce1c947dcc32ea40fd242a530e36bbf22a1790` |
| HmjeUI | `f86d7421e77349f4c053d25a836862dde89c1358` |

컴파일러·런타임·플랫폼·드라이버 소스는 패키지 툴체인과 동일한 원본 버전을 사용합니다. 이 저장소의 디렉터리 구성과 안내 진단에 적용된 차이, 나머지 라이브러리의 버전은 [소스 출처](../SOURCES.md)에 있습니다.

## 검증 결과

| 항목 | 결과 | 기록 |
| --- | --- | --- |
| 원본 소스 대조 | 검증 입력의 421개 파일 일치 | [소스 빌드 보고서](source-sync-verification.json) |
| 소스 빌드·설치 | 새 툴체인 빌드 및 별도 설치 통과 | [소스 빌드 보고서](source-sync-verification.json) |
| 스모크 검사 | 언어·화면 빌드·호스트·구문·도형 등 10개 통과 | [소스 빌드 보고서](source-sync-verification.json) |
| 개발자 세션 | 52개 항목 통과 | [예제 검증 결과](https://github.com/Semtle-Inc/hmje-developer-sessions/blob/main/docs/verification.md) |
| CPU 학습·생성 | 10걸음 학습·체크포인트 저장 및 100토큰 생성 통과 | [학습·생성 보고서](https://github.com/Semtle-Inc/hmje-developer-sessions/blob/main/docs/training-smoke.json) |
| hmje-sem | `SemTapeTest` 22개 검사 통과 | [라이브러리 안내](../libraries/hmje-sem/README.md) |
| hmje-llm | `GemmaStream` 빌드 통과 | [라이브러리 안내](../libraries/hmje-llm/README.md) |
| Swift 부트스트랩 | 49개 단계 및 네 도구의 고정점·배포 파일 비교 통과 | [재현 검증 결과](https://github.com/Semtle-Inc/hmje-swift-seed/blob/main/docs/verification-status.md) |

소스 빌드와 예제 검증은 macOS ARM64에서 수행했습니다. 보고서는 실행 당시의 소스 식별값·도구 해시·검사 목록을 기록합니다. Swift 부트스트랩 결과의 검증 코드 버전과 경로 자동 감지 기능의 별도 검사 범위는 해당 재현 검증 문서에 명시되어 있습니다.

UI 예제의 검증 범위는 앱 빌드입니다. UI 입력·화면 표시, Windows·Linux 실행, 모바일 배포와 외부 모델 추론은 위 검증에 포함되지 않습니다. 소스 빌드 결과와 서명된 배포 원본의 전체 바이트 일치는 별개이며, Swift 재현 검증은 코드 서명·UUID 정규화를 포함한 네 컴파일러 도구 비교를 대상으로 합니다.

## 라이선스

컴파일러와 도구에는 [HMJE 도구 소스 공개 라이선스](../LICENSE), 런타임과 라이브러리에는 [런타임·라이브러리 라이선스](../LICENSE-RUNTIME) 버전 1.0(2026-10-09)이 적용됩니다. Swift 씨앗 저장소에도 같은 두 라이선스가 포함됩니다.

개발자 세션의 예제·문서·학습 말뭉치는 [MIT License](https://github.com/Semtle-Inc/hmje-developer-sessions/blob/main/LICENSE)로 제공합니다. 이 라이선스는 HMJE 컴파일러·런타임·플랫폼 라이브러리의 이용 조건을 변경하지 않습니다. 포함된 제삼자 자료에는 [각 원래 라이선스](../THIRD_PARTY_NOTICES.md)가 적용됩니다.
