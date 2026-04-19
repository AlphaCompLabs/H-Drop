const R_TERRA = 6_371_000;

export function haversineMetros(lat1: number, lng1: number, lat2: number, lng2: number): number {
  const phi1 = (lat1 * Math.PI) / 180;
  const phi2 = (lat2 * Math.PI) / 180;
  const dphi = ((lat2 - lat1) * Math.PI) / 180;
  const dlam = ((lng2 - lng1) * Math.PI) / 180;
  const a =
    Math.sin(dphi / 2) ** 2 +
    Math.cos(phi1) * Math.cos(phi2) * Math.sin(dlam / 2) ** 2;
  return Math.round(2 * R_TERRA * Math.asin(Math.sqrt(a)));
}

export function formatarDistancia(metros: number): string {
  return metros >= 1000 ? `${(metros / 1000).toFixed(2)} km` : `${metros} m`;
}
