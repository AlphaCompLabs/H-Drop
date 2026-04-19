export interface ComandoDestino {
  lat: number;
  lng: number;
}

export type ComandoRapido = 'STOP' | 'GO_HOME' | 'ACK_ARRIVAL' | 'START';

export interface ComandoResposta {
  status: string;
  topico: string;
  payload: ComandoDestino;
}
