export type EstadoConexao = 'ONLINE' | 'CONECTANDO' | 'OFFLINE';

export interface StatusSistema {
  mqtt: EstadoConexao;
  websocket: EstadoConexao;
  bateriaSoC: number | null;    // 0-100 — INA226 (futuro)
  sinal4G: number | null;       // 0-100 — CSQ
  gps: 'FIXADO' | 'SEM_SINAL';
  cargaPresente: boolean | null; // HX711 (futuro)
  distanciaObstaculo: number | null; // metros (futuro)
}
