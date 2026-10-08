# hmje-network — HTTP와 네트워크 통신

HTTP 클라이언트·세션, 비동기 통신, WebSocket과 로컬 HTTP 서버를 제공합니다. 운영체제별 TLS 및 소켓 구현을 연결합니다.

## 사용

HMJE 1.0.0-beta.1 툴체인의 SDK에 포함됩니다. 소비 프로젝트의 모듈에 사용할 라이브러리를 의존성으로 선언하고, HMJE 소스에서 모듈을 가져옵니다. 내보내는 모듈은 HmjeNetwork입니다.

```hmjep
모듈 App의
    원본 경로는 "Sources/App"이고,
    의존 라이브러리는 HmjeNetwork이다.
```

```hmje
HmjeNetwork를 가져온다.
```

프로젝트·산출물의 전체 구성은 [프로젝트 명세](../../docs/project.md), 이 트리에서 SDK를 빌드·설치하는 절차는 [소스 빌드](../../docs/building.md)를 참고하십시오.

## 기능

| 구분 | 제공 내용 |
| --- | --- |
| HTTP | 요청·응답, 세션, 리다이렉트와 프록시 |
| 스트리밍 | SSE·WebSocket과 내려받기 |
| 서버 | 로컬 HTTP 요청 수신과 응답 |
| 실행 모델 | 동기·비동기 API와 연결 실패 처리 |

## 지원 범위와 사용 조건

macOS·iOS는 Apple 보안 프레임워크, Linux는 OpenSSL, Android는 플랫폼 신뢰 저장소를 사용합니다. Windows에는 별도 네이티브 구현이 있습니다. TLS 검증·연결 오류를 호출 결과로 처리하며, 비동기 작업의 취소와 자원 종료를 관리해야 합니다. 서버 노출 주소·인증·요청 제한은 앱에서 설정합니다.

## 문서와 소스

- [모듈·플랫폼별 빌드 구성](Project.hmjep)
- [구현 소스](Sources/)

## 버전과 라이선스

Beta 1의 API는 안정화 중입니다. 고정된 원본 커밋은 [소스 출처](../../SOURCES.md), 공통 변경 사항은 [릴리스 노트](../../RELEASE_NOTES-1.0.0-beta.1.md)를 확인하십시오. 앱에 포함하는 배포에는 [런타임·라이브러리 라이선스](../../LICENSE-RUNTIME)가 적용됩니다.
