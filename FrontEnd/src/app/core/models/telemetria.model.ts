export type StatusGPS = 'FIXADO' | 'SEM_SINAL';

export interface GPSProcessado {
  status: StatusGPS;
  lat: number | null;
  lng: number | null;
}

export interface Telemetria {
  proa: number;
  gps: GPSProcessado;
}
