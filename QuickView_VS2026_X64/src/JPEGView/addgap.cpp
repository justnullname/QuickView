
void CNavigationPanel::AddGap(int nID, int nGap) {
	m_controls[nID] = new CGapCtrl(this, nGap);
}
