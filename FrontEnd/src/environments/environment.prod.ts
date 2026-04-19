export const environment = {
  production: true,
  apiBaseUrl: '/api/v1',
  wsUrl: `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/api/v1/ws/telemetria`,
  gpsSmoothingWindow: 5,
};
