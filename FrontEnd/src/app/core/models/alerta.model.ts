export type Severidade = 'critical' | 'warning' | 'info' | 'ok';

export interface Alerta {
  id: string;
  severidade: Severidade;
  titulo: string;
  mensagem: string;
  timestamp: number;
  requerConfirmacao: boolean;
}
