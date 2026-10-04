{{/*
Expand the name of the chart.
*/}}
{{- define "voice-assistant.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{/*
Create a default fully qualified app name.
*/}}
{{- define "voice-assistant.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- $name := default .Chart.Name .Values.nameOverride -}}
{{- if contains $name .Release.Name -}}
{{- .Release.Name | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name $name | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}
{{- end -}}

{{/*
Common labels.
*/}}
{{- define "voice-assistant.labels" -}}
helm.sh/chart: {{ printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" }}
app.kubernetes.io/name: {{ include "voice-assistant.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end -}}

{{/*
Selector labels.
*/}}
{{- define "voice-assistant.selectorLabels" -}}
app.kubernetes.io/name: {{ include "voice-assistant.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}

{{/*
Name of the K8s Secret that holds the Hermes API key.
*/}}
{{- define "voice-assistant.hermesSecretName" -}}
{{- if .Values.hermes.apiKey.existingSecret -}}
{{- .Values.hermes.apiKey.existingSecret -}}
{{- else -}}
{{- printf "%s-hermes" (include "voice-assistant.fullname" .) -}}
{{- end -}}
{{- end -}}

{{/*
Name of the K8s Secret that holds the ElevenLabs API key.
*/}}
{{- define "voice-assistant.elevenlabsSecretName" -}}
{{- if .Values.elevenlabs.apiKey.existingSecret -}}
{{- .Values.elevenlabs.apiKey.existingSecret -}}
{{- else -}}
{{- printf "%s-elevenlabs" (include "voice-assistant.fullname" .) -}}
{{- end -}}
{{- end -}}

{{/*
Name of the K8s Secret that holds the STUNner TURN credentials.
*/}}
{{- define "voice-assistant.turnSecretName" -}}
{{- if .Values.stunner.auth.existingSecret -}}
{{- .Values.stunner.auth.existingSecret -}}
{{- else -}}
{{- printf "%s-turn" (include "voice-assistant.fullname" .) -}}
{{- end -}}
{{- end -}}

{{/*
Cluster-scoped GatewayClass name. Falls back to the release fullname.
*/}}
{{- define "voice-assistant.gatewayClassName" -}}
{{- default (include "voice-assistant.fullname" .) .Values.stunner.gatewayClassName -}}
{{- end -}}

{{/*
TLS secret name for the Ingress.
*/}}
{{- define "voice-assistant.ingressTlsSecretName" -}}
{{- default (printf "%s-tls" (include "voice-assistant.fullname" .)) .Values.ingress.tls.secretName -}}
{{- end -}}

{{/*
Validate that exactly one source is configured for the Hermes API key
and (if no existing Secret) for the STUNner TURN password.
*/}}
{{- define "voice-assistant.validate" -}}
{{- $h := .Values.hermes.apiKey -}}
{{- if and (not $h.existingSecret) (not $h.externalSecret.enabled) (not $h.value) -}}
{{- fail "Set one of: hermes.apiKey.existingSecret | hermes.apiKey.externalSecret.enabled=true | hermes.apiKey.value" -}}
{{- end -}}
{{- $s := .Values.stunner.auth -}}
{{- if and (not $s.existingSecret) (not $s.externalSecret.enabled) (not $s.password) -}}
{{- fail "Set one of: stunner.auth.existingSecret | stunner.auth.externalSecret.enabled=true | stunner.auth.password" -}}
{{- end -}}
{{- end -}}
