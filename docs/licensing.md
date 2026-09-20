# Licensing boundaries

The repository defaults to [MIT](../LICENSE). The new price optimization tool
is the explicit exception: its original files use the
[Praesidium Humanitatis Worker Protection License 1.0](../tools/price-optimization/LICENSE),
identified in source headers as
`LicenseRef-Praesidium-Humanitatis-Worker-Protection-1.0`.

| Material | Terms |
| --- | --- |
| Repository documentation, shared infrastructure, and application projects | MIT unless a specific file identifies another license |
| Temporal Fusion Transformer tool, including its existing implementation | MIT; upstream dependencies keep their own terms |
| Original files under `tools/price-optimization/`, including C++ code, AMPL model files, tests, and tool environment scripts | Worker Protection License 1.0 |
| Inherited MIT environment code, dependency pins, and attribution notice in the optimizer | Explicit MIT portions retain MIT; new script modifications also require the worker-protection terms. See the [environment notice](../tools/price-optimization/environment/NOTICE.md) |
| Independent MIT [post-profit-exchange application](../projects/post-profit-exchange/README.md) that calls the engine | Application stays MIT; using or hosting the engine still requires compliance with the engine license |
| Standalone exchange HTML containing compiled pricing code | Combined artifact includes Worker Protection engine terms, MIT application/JSON terms and compiler-runtime notices; it is not MIT-only. Corresponding source is embedded as a downloadable ZIP. No AMPL binary or entitlement is included. |
| AMPL, its SDK, solver software, and other third-party material | Their own licenses; neither the root MIT license nor the worker-protection license grants rights to these products |

The custom terms cover new original engine material and derivatives of that
material, including copies moved outside its directory. They do not retract
existing MIT grants or turn independent application code into restricted code.
Preserve an explicit MIT notice when reusing previously MIT-licensed material.
Preserve third-party notices and check compatibility before combining code;
an API boundary is not a blanket finding that every proposed combination is
legally compatible. The [third-party notices](third-party-notices.md) identify
external dependencies.

This is **source-available software with use restrictions**, not an OSI-approved
open-source license. OSI's definition disallows restrictions on fields of use;
the engine intentionally imposes such restrictions. The rest of the project
can remain MIT without representing the entire repository as MIT-only.
[Open Source Definition](https://opensource.org/osd),
[MIT license](https://opensource.org/license/mit).

## What the engine terms require

The license permits study, modification, compliant operation, distribution,
and paid services. It prohibits slavery, trafficking, forced labor, coercive
worker control, retaliation, persecution, discriminatory decisions, and defined
forms of exploitation in essential pricing. Productive deployments must give
workers equal democratic control over objectives, safeguards, compensation,
delegated authority, and surplus. No passive owner can extract that surplus.
Reasonable operating costs, fair wages, fixed financing costs without control,
reserves, and worker-approved reinvestment remain possible.

A store may have legal property held by a cooperative or trust while giving
workers the required control and benefit. Calling a business a nonprofit,
calling an operational deployment research, or wrapping the engine in an MIT
application does not waive these obligations. Human review and the power to
stop or reverse decisions are required; a fully unaccountable automated owner
would not satisfy them. Offline development and education can proceed without
constituting a productive deployment.

Distribution must preserve terms and provide covered source. A modified hosted
engine must provide its covered source to interacting users and affected workers.
Independent applications and confidential data are outside that source scope.
The familiar distinction between hosted interaction and distribution informs
the design, but this is an original custom license, not an AGPL variant and not
an assertion of AGPL compatibility.
[GNU licensing overview](https://www.gnu.org/licenses/).

Enforcement provisions include deployment records, an annual worker report,
specific-evidence compliance inquiries with redaction, immediate termination
for substantive worker-rights violations, and a limited cure period for
administrative omissions. They authorize no telemetry or unrestricted audit of
private systems. Keeping records must not create worker surveillance.

## AMPL is a separate permission boundary

For synthetic development tests, the official runtime is now staged locally
with its vendor-supplied Demo license. That mode is distinct from Community
Edition and permits only the vendor's stated evaluation, education, and
non-commercial uses within its size limits. The supplied license file was
preserved without activation or modification. No vendor binaries or license
files are included in the tracked source release.

Obtain an AMPL entitlement and any required solver entitlement for the actual
deployment. Do not assume that a free, community, academic, trial, or nonprofit
entitlement permits operation of a revenue-generating worker-controlled store.
The published AMPL EULA's commercial-prototyping terms expressly limit business
use of Community Edition results; its nonprofit offering is also described as
non-commercial. Container use and redistribution must comply with the applicable
agreement. Do not commit license keys or redistribute proprietary runtime/SDK
files without the necessary permission. A solver's open-source license does not
make AMPL itself open source. These terms were checked on 2026-09-20; confirm the
agreement applicable to the entitlement actually obtained.
[AMPL terms and EULA, sections 3 and 5](https://ampl.com/terms-eula/).

## Review status and practical limits

The custom license is a concrete **unreviewed legal draft** supplied with the
new engine. It is not a legal opinion or a promise that every clause will be
enforced in every country. No jurisdiction has been chosen. Qualified counsel
in the intended deployment and enforcement jurisdictions should review the
actual text before public release or reliance in a live store, especially
copyright-versus-contract conditions, acceptance, worker governance, remedies,
privacy, competition and pricing law, and dependency compatibility. Copyright
holders must have authority to offer every covered contribution on these terms.

Licensing gives a basis to challenge covered misuse; it cannot physically stop
someone copying code, prove consent, make worker ownership real, or substitute
for legal institutions and accountable operations. Technical caps and policy
checks are useful safeguards, but a successful solve cannot certify compliance.
Effective follow-through also needs accessible reports, reliable evidence,
rights-holder enforcement capacity, and organization rules that workers can
actually enforce. Existing MIT copies remain usable under their original grant.

The labor and governance definitions were informed by the ILO's explanation of
voluntary labor and its recommendation on democratic cooperative control and
member benefit. This project's prohibition and deployment conditions are its
own terms; they do not claim ILO approval or turn every ILO recommendation into
a statutory obligation for every user.
[ILO: forced labor](https://www.ilo.org/topics/forced-labour-modern-slavery-and-trafficking-persons/what-forced-labour),
[ILO Recommendation 193](https://normlex.ilo.org/dyn/nrmlx_en/f?p=NORMLEXPUB%3A12100%3A0%3A%3ANO%3A%3AP12100_ILO_CODE%3AR193).
